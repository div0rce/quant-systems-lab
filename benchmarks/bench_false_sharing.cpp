#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace qsl::bench {
namespace {

// 128 bytes conservatively places the padded indices on distinct coherency lines for hosts with
// 64- or 128-byte cache lines (Apple Silicon uses 128), so the "padded" case is genuinely unshared.
constexpr std::size_t kCacheLine = 128;
constexpr std::uint64_t kIterations = 2'000'000;

using clock_type = std::chrono::steady_clock;

// Aligned to a cache line so the struct starts on a line boundary: the two adjacent atomics then
// provably share one coherency line (the false-sharing baseline), rather than risking a straddle
// across two lines depending on stack/ABI placement.
struct alignas(kCacheLine) PackedQueueIndices {
    std::atomic<std::size_t> head{0};
    std::atomic<std::size_t> tail{0};
};

struct alignas(kCacheLine) CacheLineIndex {
    std::atomic<std::size_t> value{0};
};

struct PaddedQueueIndices {
    CacheLineIndex head{};
    CacheLineIndex tail{};
};

std::atomic<std::size_t> &head_index(PackedQueueIndices &indices) noexcept {
    return indices.head;
}

std::atomic<std::size_t> &tail_index(PackedQueueIndices &indices) noexcept {
    return indices.tail;
}

std::atomic<std::size_t> &head_index(PaddedQueueIndices &indices) noexcept {
    return indices.head.value;
}

std::atomic<std::size_t> &tail_index(PaddedQueueIndices &indices) noexcept {
    return indices.tail.value;
}

void wait_for_start(std::atomic<int> &ready, std::atomic<bool> &start) {
    ready.fetch_add(1, std::memory_order_acq_rel);
    while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

struct Sample {
    double seconds = 0.0;
    std::uint64_t checksum = 0;
};

template <class Indices> Sample run_index_study() {
    Indices indices{};
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::uint64_t producer_checksum = 0;
    std::uint64_t consumer_checksum = 0;

    std::thread producer{[&] {
        wait_for_start(ready, start);
        std::uint64_t checksum = 0;
        for (std::uint64_t i = 1; i <= kIterations; ++i) {
            const auto value = static_cast<std::size_t>(i);
            tail_index(indices).store(value, std::memory_order_release);
            checksum +=
                static_cast<std::uint64_t>(head_index(indices).load(std::memory_order_acquire));
        }
        producer_checksum = checksum;
    }};

    std::thread consumer{[&] {
        wait_for_start(ready, start);
        std::uint64_t checksum = 0;
        for (std::uint64_t i = 1; i <= kIterations; ++i) {
            const auto value = static_cast<std::size_t>(i);
            head_index(indices).store(value, std::memory_order_release);
            checksum +=
                static_cast<std::uint64_t>(tail_index(indices).load(std::memory_order_acquire));
        }
        consumer_checksum = checksum;
    }};

    while (ready.load(std::memory_order_acquire) != 2) {
        std::this_thread::yield();
    }

    const auto t0 = clock_type::now();
    start.store(true, std::memory_order_release);
    producer.join();
    consumer.join();
    const auto t1 = clock_type::now();

    const double seconds = std::chrono::duration<double>(t1 - t0).count();
    return {seconds, producer_checksum + consumer_checksum};
}

template <class Indices> void report(const char *name) {
    const Sample sample = run_index_study<Indices>();
    const auto updates = static_cast<double>(kIterations * 2U);
    const double ns_per_update = (sample.seconds * 1e9) / updates;
    const double updates_per_second = updates / sample.seconds;
    std::printf("%-24s %10llu cursor updates %10.1f ns/update %12.0f updates/sec checksum=%llu\n",
                name, static_cast<unsigned long long>(kIterations * 2U), ns_per_update,
                updates_per_second, static_cast<unsigned long long>(sample.checksum));
}

} // namespace

void run_false_sharing_benchmarks() {
    std::printf("SPSC cursor false-sharing study (benchmark-only control layout)\n");
    std::printf("Each thread owns one queue index, stores it with release, and samples the peer\n");
    std::printf(
        "index with acquire. Benchmark-only control: the padded layout puts each index on a\n");
    std::printf(
        "128-byte boundary, so the two cursors sit on distinct coherency lines even on hosts\n");
    std::printf(
        "with 128-byte cache lines (Apple Silicon); the production SpscRing pads to 64 bytes.\n\n");
    report<PackedQueueIndices>("packed indices");
    report<PaddedQueueIndices>("padded indices");
}

} // namespace qsl::bench
