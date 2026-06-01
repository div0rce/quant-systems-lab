#pragma once

#include "qsl/concurrency/spsc_ring.hpp"
#include "qsl/core/result.hpp"
#include "qsl/core/types.hpp"
#include "qsl/engine/events.hpp"
#include "qsl/engine/matching_engine.hpp"
#include "qsl/engine/risk.hpp"
#include "qsl/gateway/order_gateway.hpp"
#include "qsl/replay/command.hpp"
#include "qsl/replay/dispatch.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

namespace qsl::concurrency {

// Multithreaded gateway-engine-feed pipeline prototype (M26). It introduces a real concurrency
// boundary around the existing single-threaded core without changing deterministic engine
// semantics: each stage is single-threaded, stages are connected by the bounded SPSC ring (M24),
// and exactly one thread ever touches the engine.
//
//   input thread --[inbound SpscRing<Command>]--> engine thread --[outbound SpscRing<...>]--> sink
//
// Design notes:
//   - The *engine thread* is the sole owner of the MatchingEngine + OrderGateway, so the engine is
//     never shared and its semantics stay exactly as in the single-threaded path. The result is
//     therefore deterministic: the same command stream yields the same final snapshot and the same
//     ordered event stream regardless of thread timing or queue capacity (the tests assert this
//     against a single-threaded reference).
//   - Both queues use the lossless **spin/yield** backpressure policy (see
//   docs/concurrency_model.md):
//     orders must never be dropped on the inbound side, and event-log/feed records must never be
//     silently dropped on the outbound side.
//   - The downstream (publisher/log) stage is intentionally **engine-independent**: it consumes
//     self-contained `ProcessedCommand` records and never reads the engine. The M6 market-data
//     publisher derives top-of-book by reading the engine, so running it across this thread
//     boundary would race the engine thread; a downstream consumer that needs top-of-book would
//     instead read the events (or a snapshot captured on the engine thread). Keeping the sink
//     engine-free is what makes "publisher lag cannot corrupt engine state" structurally true.

/// One unit handed from the engine thread to the publisher/log thread: the command that was
/// applied, the gateway's accept/reject outcome, and the engine events it produced. Self-contained
/// so the downstream stage never needs to touch the (concurrently-mutating) engine.
struct ProcessedCommand {
    replay::Command command{};
    bool accepted = false;
    core::RejectReason reason = core::RejectReason::None;
    std::vector<engine::EngineEvent> events{};
};

/// Downstream "publisher/log" stage. `on_processed` is called once per command the engine
/// processed, in engine order, on the publisher/log thread. Implementations MUST NOT touch the
/// engine (owned by the engine thread) and must surface their own write failures rather than
/// silently dropping records.
class OutputSink {
  public:
    virtual ~OutputSink() = default;
    virtual void on_processed(const ProcessedCommand &processed) = 0;
};

/// Outcome of a pipeline run. The engine-thread fields are captured after the engine has drained
/// its input and is quiescent (still owned by the engine thread); the main thread fills the
/// backpressure counters after all threads are joined, so every field is read race-free.
struct PipelineResult {
    engine::EngineSnapshot snapshot{};
    std::uint64_t commands_processed = 0;
    std::uint64_t commands_accepted = 0;
    std::uint64_t commands_rejected = 0;
    std::uint64_t events_emitted = 0;
    core::SeqNo last_seq = 0;
    std::uint64_t inbound_backpressure_spins =
        0; // times the input thread spun on a full inbound queue
    std::uint64_t outbound_backpressure_spins =
        0; // times the engine thread spun on a full outbound queue
};

/// Bounded three-thread pipeline. `InboundCapacity`/`OutboundCapacity` are the SPSC queue
/// capacities (compile-time); small values force backpressure in tests, larger values absorb
/// bursts. Capacity affects only timing/backpressure, never the result.
template <std::size_t InboundCapacity = 1024, std::size_t OutboundCapacity = 1024>
class ThreadedPipeline {
  public:
    /// Run `commands` through the pipeline to completion and return the engine's final snapshot
    /// plus stats. Blocks until all three threads are joined (so the queues, which live on this
    /// stack frame, outlive both the producer and the consumer of each — the lifetime bracket the
    /// SPSC contract requires). Deterministic given `commands` and `risk`.
    static PipelineResult run(const std::vector<replay::Command> &commands, engine::RiskConfig risk,
                              OutputSink &sink) {
        SpscRing<replay::Command, InboundCapacity> inbound;
        SpscRing<ProcessedCommand, OutboundCapacity> outbound;

        // Out-of-band shutdown signalling (docs/concurrency_model.md → Shutdown): the queues have
        // no "closed" state, so each upstream stage publishes a done-flag with release after its
        // last push, and the downstream stage drains-then-stops on acquire.
        std::atomic<bool> input_done{false};
        std::atomic<bool> engine_done{false};
        std::atomic<std::uint64_t> inbound_spins{0};
        std::atomic<std::uint64_t> outbound_spins{0};

        PipelineResult result{};

        // Stage 1 — input thread: the sole producer of the inbound queue.
        std::thread input_thread([&] {
            for (const replay::Command &command : commands) {
                while (!inbound.try_push(command)) { // lossless: an order is never dropped
                    inbound_spins.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::yield();
                }
            }
            input_done.store(true, std::memory_order_release);
        });

        // Stage 2 — engine thread: sole consumer of inbound, sole producer of outbound, and the
        // ONLY thread that touches the engine + gateway.
        std::thread engine_thread([&] {
            engine::MatchingEngine engine;
            gateway::OrderGateway gw(engine, risk);

            const auto process = [&](replay::Command &&command) {
                gateway::GatewayResult gr = replay::apply_command(engine, gw, command);
                ProcessedCommand pc;
                pc.command = std::move(command);
                pc.accepted = gr.accepted;
                pc.reason = gr.reason;
                pc.events = std::move(gr.events);

                ++result.commands_processed;
                if (pc.accepted) {
                    ++result.commands_accepted;
                } else {
                    ++result.commands_rejected;
                }
                result.events_emitted += pc.events.size();

                while (!outbound.try_push(
                    std::move(pc))) { // lossless: a log/feed record is never dropped
                    outbound_spins.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::yield();
                }
            };

            replay::Command command;
            for (;;) {
                if (inbound.try_pop(command)) {
                    process(std::move(command));
                    continue;
                }
                if (input_done.load(std::memory_order_acquire)) {
                    // Drain-then-stop: everything the input thread published happens-before the
                    // done-flag, so this final drain cannot miss a queued command.
                    while (inbound.try_pop(command)) {
                        process(std::move(command));
                    }
                    break;
                }
                std::this_thread::yield();
            }

            // The engine has drained its input and is quiescent; still owned by this thread, so
            // snapshotting it here is race-free.
            result.snapshot = engine.snapshot();
            result.last_seq = engine.last_seq();
            engine_done.store(true,
                              std::memory_order_release); // released after the last outbound push
        });

        // Stage 3 — publisher/log thread: sole consumer of the outbound queue.
        std::thread output_thread([&] {
            ProcessedCommand pc;
            for (;;) {
                if (outbound.try_pop(pc)) {
                    sink.on_processed(pc);
                    continue;
                }
                if (engine_done.load(std::memory_order_acquire)) {
                    while (outbound.try_pop(pc)) { // drain-then-stop: no published record is lost
                        sink.on_processed(pc);
                    }
                    break;
                }
                std::this_thread::yield();
            }
        });

        input_thread.join();
        engine_thread.join();
        output_thread.join();

        // All threads joined: these reads are sequenced after every write to them.
        result.inbound_backpressure_spins = inbound_spins.load(std::memory_order_relaxed);
        result.outbound_backpressure_spins = outbound_spins.load(std::memory_order_relaxed);
        return result;
    }
};

} // namespace qsl::concurrency
