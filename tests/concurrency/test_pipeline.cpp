// Behavioral evidence for the M26 threaded gateway-engine-feed pipeline
// (include/qsl/concurrency/pipeline.hpp, docs/concurrency_model.md → "Realized pipeline").
//
// The pipeline introduces a real concurrency boundary (input thread -> inbound SPSC -> engine
// thread -> outbound SPSC -> publisher/log thread). The central correctness claim is that this
// boundary does NOT change deterministic engine semantics: for any command stream, the threaded
// run must converge to the SAME final snapshot and the SAME ordered event stream as a
// single-threaded reference, regardless of queue capacity or thread timing. These tests pin that,
// plus lossless backpressure, drain-then-stop shutdown, and event-log integrity under concurrency.
// Run under `make asan` as well; ASan/UBSan do not detect data races (that is M27/TSan), but they
// do catch any use-after-free / out-of-bounds in the threaded paths.

#include "qsl/concurrency/pipeline.hpp"
#include "qsl/engine/events.hpp"
#include "qsl/engine/matching_engine.hpp"
#include "qsl/engine/risk.hpp"
#include "qsl/gateway/order_gateway.hpp"
#include "qsl/replay/command.hpp"
#include "qsl/replay/dispatch.hpp"
#include "qsl/replay/event_log.hpp"
#include "qsl/replay/recovery.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <variant>
#include <vector>

using qsl::concurrency::OutputSink;
using qsl::concurrency::PipelineProbe;
using qsl::concurrency::PipelineResult;
using qsl::concurrency::ProcessedCommand;
using qsl::concurrency::ThreadedPipeline;
using qsl::engine::EngineEvent;
using qsl::engine::EngineSnapshot;
using qsl::engine::MatchingEngine;
using qsl::engine::RiskConfig;
using qsl::engine::TradeEvent;
using qsl::gateway::OrderGateway;
using qsl::replay::Command;

namespace {

// A low max order quantity so the property flow's oversized orders produce real risk rejections.
constexpr RiskConfig kRisk{10, 1'000'000};

// Single-threaded ground truth: drive the same command stream through one gateway+engine and
// record exactly what the pipeline must reproduce.
struct Reference {
    EngineSnapshot snapshot;
    std::vector<EngineEvent> events;
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;
    std::uint64_t events_count = 0;
    qsl::core::SeqNo last_seq = 0;
};

Reference run_reference(const std::vector<Command> &commands, RiskConfig risk) {
    MatchingEngine engine;
    OrderGateway gw(engine, risk);
    Reference r;
    for (const Command &c : commands) {
        const auto gr = qsl::replay::apply_command(engine, gw, c);
        if (gr.accepted) {
            ++r.accepted;
        } else {
            ++r.rejected;
        }
        r.events_count += gr.events.size();
        for (const EngineEvent &e : gr.events) {
            r.events.push_back(e);
        }
    }
    r.snapshot = engine.snapshot();
    r.last_seq = engine.last_seq();
    return r;
}

std::uint64_t count_trades(const std::vector<EngineEvent> &events) {
    std::uint64_t n = 0;
    for (const EngineEvent &e : events) {
        if (std::holds_alternative<TradeEvent>(e)) {
            ++n;
        }
    }
    return n;
}

// Collects the downstream event stream in order. Touched only by the publisher/log thread during
// the run, then by the test after join() — no synchronization needed.
struct CollectingSink final : OutputSink {
    std::vector<EngineEvent> events;
    std::uint64_t processed = 0;
    void on_processed(const ProcessedCommand &pc) override {
        ++processed;
        for (const EngineEvent &e : pc.events) {
            events.push_back(e);
        }
    }
};

// Deliberately slow consumer: yields on every command to simulate a lagging publisher/log stage,
// forcing the bounded outbound queue full and back-pressuring the engine thread.
struct LaggySink final : OutputSink {
    std::vector<EngineEvent> events;
    void on_processed(const ProcessedCommand &pc) override {
        std::this_thread::yield();
        for (const EngineEvent &e : pc.events) {
            events.push_back(e);
        }
    }
};

// Persists every accepted command as a Command log record using the real M7 framing, into an
// in-memory buffer (no disk dependency). Logging only accepted commands keeps the log replayable:
// an accepted gateway call applies to the engine identically to recovery's engine-direct replay,
// while rejected commands never mutated state. `ok` surfaces any append failure so nothing is
// dropped silently.
struct CommandLogSink final : OutputSink {
    std::vector<std::byte> bytes;
    qsl::core::SeqNo seq = 0;
    std::uint64_t records = 0;
    bool ok = true;
    void on_processed(const ProcessedCommand &pc) override {
        if (!pc.accepted) {
            return;
        }
        qsl::replay::LogRecord rec;
        rec.seq_no = ++seq;
        rec.type = qsl::replay::RecordType::Command;
        rec.logical_timestamp = seq; // logical, deterministic; replay ignores it
        rec.payload = qsl::replay::encode_command(pc.command);
        if (qsl::replay::encode_record(rec, bytes)) {
            ++records;
        } else {
            ok = false;
        }
    }
};

// Deterministic backpressure barrier: blocks the FIRST processed command until `open` is set, so
// the output thread parks, the engine fills the bounded outbound queue, and (with the engine
// stalled) the inbound queue fills too. A test pairs this with a PipelineProbe and releases only
// once a real spin has been observed, making the "backpressure occurred" assertion deterministic
// rather than timing-dependent.
struct GatedSink final : OutputSink {
    std::atomic<bool> open{false};
    std::vector<EngineEvent> events;
    void on_processed(const ProcessedCommand &pc) override {
        while (!open.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (const EngineEvent &e : pc.events) {
            events.push_back(e);
        }
    }
};

// Run the pipeline at the given capacities and assert it reproduces the single-threaded reference
// exactly: same counts, same final snapshot (which includes last_seq), and the same ordered event
// stream observed by the downstream stage.
template <std::size_t In, std::size_t Out>
void require_matches_reference(const std::vector<Command> &commands, RiskConfig risk) {
    const Reference ref = run_reference(commands, risk);
    CollectingSink sink;
    const PipelineResult result = ThreadedPipeline<In, Out>::run(commands, risk, sink);

    REQUIRE(result.commands_processed == commands.size());
    REQUIRE(result.commands_accepted == ref.accepted);
    REQUIRE(result.commands_rejected == ref.rejected);
    REQUIRE(result.events_emitted == ref.events_count);
    REQUIRE(result.last_seq == ref.last_seq);
    REQUIRE(result.snapshot == ref.snapshot);
    REQUIRE(sink.processed == commands.size());
    REQUIRE(sink.events == ref.events); // exact in-order event stream
}

} // namespace

TEST_CASE("pipeline reproduces the single-threaded result, in order", "[pipeline]") {
    // GTC synthetic flow across several seeds, at both a generous capacity and a tiny capacity that
    // forces heavy backpressure in both directions. Same result either way (timing-independent).
    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
        const auto flow = qsl::replay::generate_flow(seed, 3, 400);
        require_matches_reference<1024, 1024>(flow, kRisk);
        require_matches_reference<2, 2>(flow, kRisk);
    }
}

TEST_CASE("pipeline reproduces the result on property flows (rejects, IOC, multi-symbol)",
          "[pipeline]") {
    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
        const auto flow = qsl::replay::generate_property_flow(seed, 3, 300);
        require_matches_reference<1024, 1024>(flow, kRisk);
        require_matches_reference<4, 4>(flow, kRisk);
    }
}

TEST_CASE("pipeline corpus is non-vacuous: real rejects, trades, and accepts", "[pipeline][meta]") {
    // Guards against future generator changes silently making the pipeline tests trivial.
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;
    std::uint64_t trades = 0;
    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
        const Reference ref =
            run_reference(qsl::replay::generate_property_flow(seed, 3, 300), kRisk);
        accepted += ref.accepted;
        rejected += ref.rejected;
        trades += count_trades(ref.events);
    }
    REQUIRE(accepted > 0);
    REQUIRE(rejected > 0);
    REQUIRE(trades > 0);
}

TEST_CASE("saturated inbound queue stays lossless (tiny capacity, deterministic outcome)",
          "[pipeline][backpressure]") {
    // Tiny inbound queue + a fast downstream: the input thread (cheap command copies) outruns the
    // engine (risk + matching), so the inbound queue saturates and the input thread spins on the
    // lossless spin/yield policy. The outcome must be identical to the unconstrained run --
    // capacity changes only backpressure, never the result. Whether a spin is *observed* on any
    // given run is timing-dependent and intentionally not asserted here (it can legitimately be 0
    // on a fast run); that a spin provably occurs is covered deterministically by the
    // gated-consumer test below.
    const auto flow = qsl::replay::generate_property_flow(7, 4, 800);
    const Reference ref = run_reference(flow, kRisk);

    CollectingSink sink;
    const PipelineResult result = ThreadedPipeline<2, 4096>::run(flow, kRisk, sink);

    REQUIRE(result.commands_processed == flow.size());
    REQUIRE(result.snapshot == ref.snapshot);
    REQUIRE(sink.events == ref.events);
}

TEST_CASE("publisher lag does not corrupt engine state", "[pipeline][backpressure]") {
    // A slow (yielding) downstream stage + a tiny outbound queue: the engine repeatedly fills the
    // outbound queue and back-pressures on try_push. The engine state must still match the
    // reference exactly -- a lagging publisher applies backpressure, it does not corrupt the
    // engine. A spin is not asserted here because yield() may let the consumer keep pace on a fast
    // run; backpressure is proven deterministically by the gated-consumer test below.
    const auto flow = qsl::replay::generate_property_flow(7, 4, 800);
    const Reference ref = run_reference(flow, kRisk);

    LaggySink sink;
    const PipelineResult result = ThreadedPipeline<4096, 2>::run(flow, kRisk, sink);

    REQUIRE(result.commands_processed == flow.size());
    REQUIRE(result.snapshot == ref.snapshot);
    REQUIRE(result.last_seq == ref.last_seq);
    REQUIRE(sink.events == ref.events);
}

TEST_CASE("backpressure is real and lossless: a blocked consumer forces both queues full",
          "[pipeline][backpressure]") {
    // Deterministic backpressure proof (replaces a timing-dependent ">= 1 spins" check). The gated
    // consumer parks on its first command, so the engine fills the capacity-2 outbound queue and
    // must spin; with the engine stalled it stops draining the capacity-2 inbound queue, so the
    // input thread fills it and must spin too. We wait on a live PipelineProbe until BOTH spins
    // have provably happened, *then* release the consumer -- a real barrier, not a race. The run
    // must still be lossless and uncorrupted under sustained backpressure.
    const auto flow = qsl::replay::generate_property_flow(7, 4, 800);
    const Reference ref = run_reference(flow, kRisk);

    GatedSink sink;
    PipelineProbe probe;
    PipelineResult result{};
    std::thread runner([&] { result = ThreadedPipeline<2, 2>::run(flow, kRisk, sink, &probe); });

    while (probe.inbound_spins.load(std::memory_order_relaxed) < 1 ||
           probe.outbound_spins.load(std::memory_order_relaxed) < 1) {
        std::this_thread::yield(); // park until backpressure has provably occurred on both queues
    }
    sink.open.store(true, std::memory_order_release); // now let the blocked consumer drain
    runner.join();

    REQUIRE(result.inbound_backpressure_spins >= 1); // proven by the barrier above, not raced
    REQUIRE(result.outbound_backpressure_spins >= 1);
    REQUIRE(result.commands_processed == flow.size());
    REQUIRE(result.snapshot ==
            ref.snapshot); // lossless and uncorrupted under sustained backpressure
    REQUIRE(sink.events == ref.events);
}

TEST_CASE("shutdown_empty: no commands -> clean join, empty result", "[pipeline][shutdown]") {
    const std::vector<Command> empty;
    CollectingSink sink;
    const PipelineResult result = ThreadedPipeline<8, 8>::run(empty, kRisk, sink);

    REQUIRE(result.commands_processed == 0);
    REQUIRE(result.commands_accepted == 0);
    REQUIRE(result.commands_rejected == 0);
    REQUIRE(result.events_emitted == 0);
    REQUIRE(result.last_seq == 0);
    REQUIRE(result.snapshot.symbols.empty());
    REQUIRE(sink.processed == 0);
    REQUIRE(sink.events.empty());
}

TEST_CASE("shutdown_with_pending_commands: queued commands are drained, not dropped",
          "[pipeline][shutdown]") {
    // Tiny inbound queue + slow downstream: when the input thread finishes pushing and flips
    // input_done, commands are still queued and the engine is behind. Drain-then-stop must process
    // every one of them before the engine thread exits.
    const auto flow = qsl::replay::generate_property_flow(3, 3, 400);
    const Reference ref = run_reference(flow, kRisk);

    LaggySink sink;
    const PipelineResult result = ThreadedPipeline<2, 8>::run(flow, kRisk, sink);

    REQUIRE(result.commands_processed == flow.size());
    REQUIRE(result.commands_accepted == ref.accepted);
    REQUIRE(result.commands_rejected == ref.rejected);
    REQUIRE(result.snapshot == ref.snapshot);
    REQUIRE(sink.events == ref.events);
}

TEST_CASE("shutdown_with_full_queue: full queues at shutdown lose nothing",
          "[pipeline][shutdown]") {
    // Both queues tiny + slow downstream: both the inbound and outbound queues are saturated around
    // the moment shutdown is signalled. Nothing may be lost at the boundary.
    const auto flow = qsl::replay::generate_property_flow(5, 3, 400);
    const Reference ref = run_reference(flow, kRisk);

    LaggySink sink;
    const PipelineResult result = ThreadedPipeline<2, 2>::run(flow, kRisk, sink);

    REQUIRE(result.commands_processed == flow.size());
    REQUIRE(result.last_seq == ref.last_seq);
    REQUIRE(result.snapshot == ref.snapshot);
    REQUIRE(sink.events == ref.events);
}

TEST_CASE("event-log integrity: accepted commands are logged and replay to the same state",
          "[pipeline][event-log]") {
    // The publisher/log thread persists every accepted command via the real M7 log framing while
    // the engine runs concurrently. Nothing may be silently dropped, and replaying the
    // concurrently-written log into a fresh engine must reproduce the pipeline's final state.
    const auto flow = qsl::replay::generate_property_flow(7, 3, 500);
    const Reference ref = run_reference(flow, kRisk);

    CommandLogSink sink;
    const PipelineResult result =
        ThreadedPipeline<4, 4>::run(flow, kRisk, sink); // small caps: backpressure, still lossless

    REQUIRE(sink.ok);                      // no append failed silently
    REQUIRE(sink.records == ref.accepted); // every accepted command logged, none dropped

    const auto log = qsl::replay::read_log(sink.bytes);
    REQUIRE(log.error == qsl::replay::LogError::None); // decodes cleanly on a record boundary
    REQUIRE(log.records.size() == ref.accepted);

    MatchingEngine replayed;
    const auto replayed_events = qsl::replay::replay(replayed, log.records);
    REQUIRE(replayed_events == ref.events); // the logged stream replays the same events ...
    REQUIRE(replayed.snapshot() == result.snapshot); // ... log replay == pipeline state ...
    REQUIRE(replayed.snapshot() == ref.snapshot);    // ... == single-threaded reference
}
