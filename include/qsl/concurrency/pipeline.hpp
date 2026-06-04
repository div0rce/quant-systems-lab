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

/// Optional live observation hook. The pipeline increments these counters *as backpressure happens*
/// (not only at the end), so a test can deterministically wait until backpressure has provably
/// occurred — e.g. before releasing a gated consumer — instead of asserting on a timing-dependent
/// final spin count. Production callers pass `nullptr`.
struct PipelineProbe {
    std::atomic<std::uint64_t> inbound_spins{0};
    std::atomic<std::uint64_t> outbound_spins{0};
};

/// Optional deterministic scheduling perturbation. Tests can ask each stage to yield after every N
/// successful steps, exploring more interleavings without making correctness depend on wall-clock
/// sleeps or scheduler timing. Normal callers pass `nullptr`.
struct PipelinePerturbation {
    std::uint32_t input_yield_every = 0;
    std::uint32_t engine_yield_every = 0;
    std::uint32_t output_yield_every = 0;
    std::uint32_t yields_per_hit = 1;
};

struct PipelineRunOptions {
    PipelineProbe *probe = nullptr;
    const PipelinePerturbation *perturbation = nullptr;
};

namespace detail {
inline void maybe_yield(std::uint64_t &counter, std::uint32_t every, std::uint32_t yields) {
    if (every == 0) {
        return;
    }
    ++counter;
    if ((counter % every) != 0) {
        return;
    }
    for (std::uint32_t i = 0; i < yields; ++i) {
        std::this_thread::yield();
    }
}
} // namespace detail

/// Bounded three-thread pipeline. `InboundCapacity`/`OutboundCapacity` are the SPSC queue
/// capacities (compile-time); small values force backpressure in tests, larger values absorb
/// bursts. Capacity affects only timing/backpressure, never the result.
template <std::size_t InboundCapacity = 1024, std::size_t OutboundCapacity = 1024>
class ThreadedPipeline {
    struct RunContext {
        RunContext(const std::vector<replay::Command> &commands_in, engine::RiskConfig risk_in,
                   OutputSink &sink_in, PipelineRunOptions options)
            : commands(commands_in), risk(risk_in), sink(sink_in),
              perturbation(options.perturbation),
              spins(options.probe != nullptr ? *options.probe : local_probe),
              inbound_spins_base(spins.inbound_spins.load(std::memory_order_relaxed)),
              outbound_spins_base(spins.outbound_spins.load(std::memory_order_relaxed)) {}

        const std::vector<replay::Command> &commands;
        engine::RiskConfig risk;
        OutputSink &sink;
        const PipelinePerturbation *perturbation;
        SpscRing<replay::Command, InboundCapacity> inbound{};
        SpscRing<ProcessedCommand, OutboundCapacity> outbound{};
        std::atomic<bool> input_done{false};
        std::atomic<bool> engine_done{false};
        PipelineProbe local_probe{};
        PipelineProbe &spins;
        std::uint64_t inbound_spins_base = 0;
        std::uint64_t outbound_spins_base = 0;
        PipelineResult result{};
    };

    struct EngineStage {
        explicit EngineStage(engine::RiskConfig risk) : gw(engine, risk) {}

        engine::MatchingEngine engine;
        gateway::OrderGateway gw;
        std::uint64_t steps = 0;
    };

    static void count_spin(std::atomic<std::uint64_t> &counter) {
        counter.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::yield();
    }

    static void maybe_input_yield(RunContext &ctx, std::uint64_t &steps) {
        if (ctx.perturbation != nullptr) {
            detail::maybe_yield(steps, ctx.perturbation->input_yield_every,
                                ctx.perturbation->yields_per_hit);
        }
    }

    static void maybe_engine_yield(RunContext &ctx, std::uint64_t &steps) {
        if (ctx.perturbation != nullptr) {
            detail::maybe_yield(steps, ctx.perturbation->engine_yield_every,
                                ctx.perturbation->yields_per_hit);
        }
    }

    static void maybe_output_yield(RunContext &ctx, std::uint64_t &steps) {
        if (ctx.perturbation != nullptr) {
            detail::maybe_yield(steps, ctx.perturbation->output_yield_every,
                                ctx.perturbation->yields_per_hit);
        }
    }

    static void push_inbound_lossless(RunContext &ctx, const replay::Command &command) {
        while (!ctx.inbound.try_push(command)) { // lossless: an order is never dropped
            count_spin(ctx.spins.inbound_spins);
        }
    }

    static void push_outbound_lossless(RunContext &ctx, ProcessedCommand &&processed) {
        while (!ctx.outbound.try_push(
            std::move(processed))) { // lossless: a log/feed record is never dropped
            count_spin(ctx.spins.outbound_spins);
        }
    }

    static ProcessedCommand apply_to_engine(RunContext &ctx, engine::MatchingEngine &engine,
                                            gateway::OrderGateway &gw, replay::Command &&command) {
        gateway::GatewayResult gr = replay::apply_command(engine, gw, command);
        ProcessedCommand processed;
        processed.command = std::move(command);
        processed.accepted = gr.accepted;
        processed.reason = gr.reason;
        processed.events = std::move(gr.events);

        ++ctx.result.commands_processed;
        if (processed.accepted) {
            ++ctx.result.commands_accepted;
        } else {
            ++ctx.result.commands_rejected;
        }
        ctx.result.events_emitted += processed.events.size();
        return processed;
    }

    static void process_engine_command(RunContext &ctx, EngineStage &stage,
                                       replay::Command &&command) {
        push_outbound_lossless(ctx,
                               apply_to_engine(ctx, stage.engine, stage.gw, std::move(command)));
        maybe_engine_yield(ctx, stage.steps);
    }

    static void drain_inbound_after_done(RunContext &ctx, EngineStage &stage,
                                         replay::Command &command) {
        // Drain-then-stop: everything the input thread published happens-before the done-flag, so
        // this final drain cannot miss a queued command.
        while (ctx.inbound.try_pop(command)) {
            process_engine_command(ctx, stage, std::move(command));
        }
    }

    static void publish_processed(RunContext &ctx, const ProcessedCommand &processed,
                                  std::uint64_t &output_steps) {
        ctx.sink.on_processed(processed);
        maybe_output_yield(ctx, output_steps);
    }

    static void drain_outbound_after_done(RunContext &ctx, ProcessedCommand &processed,
                                          std::uint64_t &output_steps) {
        while (ctx.outbound.try_pop(processed)) { // drain-then-stop: no published record is lost
            publish_processed(ctx, processed, output_steps);
        }
    }

    static void run_input_stage(RunContext &ctx) {
        std::uint64_t input_steps = 0;
        for (const replay::Command &command : ctx.commands) {
            push_inbound_lossless(ctx, command);
            maybe_input_yield(ctx, input_steps);
        }
        ctx.input_done.store(true, std::memory_order_release);
    }

    static void run_engine_stage(RunContext &ctx) {
        EngineStage stage(ctx.risk);
        replay::Command command;

        for (;;) {
            if (ctx.inbound.try_pop(command)) {
                process_engine_command(ctx, stage, std::move(command));
                continue;
            }
            if (ctx.input_done.load(std::memory_order_acquire)) {
                drain_inbound_after_done(ctx, stage, command);
                break;
            }
            std::this_thread::yield();
        }

        // The engine has drained its input and is quiescent; still owned by this thread, so
        // snapshotting it here is race-free.
        ctx.result.snapshot = stage.engine.snapshot();
        ctx.result.last_seq = stage.engine.last_seq();
        ctx.engine_done.store(true,
                              std::memory_order_release); // released after the last outbound push
    }

    static void run_output_stage(RunContext &ctx) {
        std::uint64_t output_steps = 0;
        ProcessedCommand processed;

        for (;;) {
            if (ctx.outbound.try_pop(processed)) {
                publish_processed(ctx, processed, output_steps);
                continue;
            }
            if (ctx.engine_done.load(std::memory_order_acquire)) {
                drain_outbound_after_done(ctx, processed, output_steps);
                break;
            }
            std::this_thread::yield();
        }
    }

    static PipelineResult finish(RunContext &ctx) {
        // All threads joined: these reads are sequenced after every write to them.
        const auto inbound_spins_now = ctx.spins.inbound_spins.load(std::memory_order_relaxed);
        const auto outbound_spins_now = ctx.spins.outbound_spins.load(std::memory_order_relaxed);
        ctx.result.inbound_backpressure_spins = inbound_spins_now - ctx.inbound_spins_base;
        ctx.result.outbound_backpressure_spins = outbound_spins_now - ctx.outbound_spins_base;
        return ctx.result;
    }

  public:
    /// Run `commands` through the pipeline to completion and return the engine's final snapshot
    /// plus stats. Blocks until all three threads are joined (so the queues, which live on this
    /// stack frame, outlive both the producer and the consumer of each — the lifetime bracket the
    /// SPSC contract requires). Deterministic given `commands` and `risk`.
    static PipelineResult run(const std::vector<replay::Command> &commands, engine::RiskConfig risk,
                              OutputSink &sink, PipelineRunOptions options = {}) {
        RunContext ctx(commands, risk, sink, options);
        std::thread input_thread([&] { run_input_stage(ctx); });
        std::thread engine_thread([&] { run_engine_stage(ctx); });
        std::thread output_thread([&] { run_output_stage(ctx); });

        input_thread.join();
        engine_thread.join();
        output_thread.join();
        return finish(ctx);
    }
};

} // namespace qsl::concurrency
