#include "qsl/engine/matching_engine.hpp"
#include "qsl/protocol/endian.hpp"
#include "qsl/replay/command.hpp"
#include "qsl/replay/event_log.hpp"
#include "qsl/replay/recovery.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

constexpr std::string_view kUsage =
    "usage:\n"
    "  qsl-replay generate <file> [seed]\n"
    "  qsl-replay recover <file> [--repair]\n"
    "  qsl-replay append-loop <file> <buffered|flush|fsync> [max_records]\n"
    "  qsl-replay <file>\n";

const char *to_string(qsl::replay::LogError error) noexcept {
    switch (error) {
    case qsl::replay::LogError::None:
        return "ok";
    case qsl::replay::LogError::OpenFailed:
        return "open-failed";
    case qsl::replay::LogError::Truncated:
        return "truncated";
    case qsl::replay::LogError::BadChecksum:
        return "bad-checksum";
    case qsl::replay::LogError::PayloadTooLarge:
        return "payload-too-large";
    }
    return "unknown";
}

const char *to_string(qsl::replay::TailState tail) noexcept {
    switch (tail) {
    case qsl::replay::TailState::CleanTail:
        return "clean";
    case qsl::replay::TailState::TornTail:
        return "torn";
    case qsl::replay::TailState::Corrupt:
        return "corrupt";
    }
    return "unknown";
}

std::optional<std::uint64_t> parse_u64(std::string_view value) {
    std::uint64_t parsed = 0;
    const auto *begin = value.data();
    const auto *end = begin + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<qsl::replay::DurabilityMode> parse_mode(std::string_view value) {
    if (value == "buffered") {
        return qsl::replay::DurabilityMode::BufferedOnly;
    }
    if (value == "flush") {
        return qsl::replay::DurabilityMode::FlushOnAppend;
    }
    if (value == "fsync") {
        return qsl::replay::DurabilityMode::FsyncOnAppend;
    }
    return std::nullopt;
}

int generate(const std::string &path, std::uint64_t seed) {
    const auto flow = qsl::replay::generate_flow(seed, /*symbols=*/4, /*orders=*/500);
    qsl::replay::EventLogWriter writer{path};
    if (!writer.good()) {
        std::cerr << "cannot open " << path << " for writing\n";
        return 1;
    }
    std::uint64_t seq = 0;
    for (const auto &command : flow) {
        if (!writer.append(qsl::replay::LogRecord{seq, qsl::replay::RecordType::CommandRecord, seq,
                                                  qsl::replay::encode_command(command)})) {
            std::cerr << "append failed for " << path << " at record " << seq << "\n";
            return 1;
        }
        ++seq;
    }
    std::cout << "wrote " << flow.size() << " command records to " << path << "\n";
    return 0;
}

// Crash-recovery inspection: report the longest valid prefix and the tail classification,
// optionally truncating a torn tail back to the last valid record boundary. Exit 0 means the
// log is clean after the operation; corrupt logs are never repaired by automation.
int recover(const std::string &path, bool repair) {
    const auto recovery = qsl::replay::recover_log_file(path);
    if (recovery.error != qsl::replay::LogError::None) {
        std::cerr << "log read error: " << to_string(recovery.error) << "\n";
        return 1;
    }
    std::cout << "records: " << recovery.records.size() << "\n";
    std::cout << "valid_bytes: " << recovery.valid_bytes << "\n";
    std::cout << "tail: " << to_string(recovery.tail) << "\n";
    std::cout << "tail_error: " << to_string(recovery.tail_error) << "\n";
    if (recovery.tail == qsl::replay::TailState::CleanTail) {
        return 0;
    }
    if (recovery.tail == qsl::replay::TailState::Corrupt) {
        std::cerr << "damage is not confined to the final record; refusing automated repair\n";
        return 1;
    }
    if (!repair) {
        std::cerr << "torn tail found; rerun with --repair to truncate it\n";
        return 1;
    }
    if (!qsl::replay::repair_log_file(path, recovery)) {
        std::cerr << "repair failed\n";
        return 1;
    }
    std::cout << "repaired: truncated to " << recovery.valid_bytes << " bytes\n";
    return 0;
}

// Append deterministic records until killed (or until max_records when nonzero), printing an
// `ack <seq>` line after each append the writer reported as complete. A crash harness can
// SIGKILL this process mid-stream and compare the acknowledged count against what recovery
// finds in the file.
int append_loop(const std::string &path, qsl::replay::DurabilityMode mode,
                std::uint64_t max_records) {
    qsl::replay::EventLogWriter writer{path, mode};
    if (!writer.good()) {
        std::cerr << "cannot open " << path << " for writing\n";
        return 1;
    }
    for (std::uint64_t seq = 1; max_records == 0 || seq <= max_records; ++seq) {
        std::vector<std::byte> payload(16);
        qsl::protocol::store_be<std::uint64_t>(payload.data(), seq);
        qsl::protocol::store_be<std::uint64_t>(payload.data() + 8, seq ^ 0xA5A5A5A5A5A5A5A5ULL);
        if (!writer.append(qsl::replay::LogRecord{seq, qsl::replay::RecordType::CommandRecord, seq,
                                                  std::move(payload)})) {
            std::cerr << "append failed for " << path << " at record " << seq << "\n";
            return 1;
        }
        std::cout << "ack " << seq << "\n" << std::flush;
    }
    return 0;
}

int replay(const std::string &path) {
    const qsl::replay::EventLogReader reader{path};
    const auto log = reader.read_all();
    if (log.error != qsl::replay::LogError::None) {
        std::cerr << "log read error: " << to_string(log.error) << "\n";
        return 1;
    }
    qsl::engine::MatchingEngine engine;
    const auto events = qsl::replay::replay(engine, log.records);
    const auto snapshot = engine.snapshot();

    std::cout << "records: " << log.records.size() << "\n";
    std::cout << "replayed events: " << events.size() << "\n";
    std::cout << "last_seq: " << snapshot.last_seq << "\n";
    for (const auto &sym : snapshot.symbols) {
        std::cout << "symbol " << sym.symbol << ": orders=" << sym.order_count
                  << " bid=" << (sym.best_bid ? std::to_string(*sym.best_bid) : "-")
                  << " ask=" << (sym.best_ask ? std::to_string(*sym.best_ask) : "-") << "\n";
    }
    return 0;
}

} // namespace

// qsl-replay generate <file> [seed]                        -> write a synthetic-flow command log
// qsl-replay recover <file> [--repair]                     -> classify and optionally repair a log
// qsl-replay append-loop <file> <mode> [max_records]       -> append until killed (crash harness)
// qsl-replay <file>                                        -> rebuild engine state from the log
int main(int argc, char **argv) {
    if (argc >= 3 && std::string(argv[1]) == "generate") {
        std::uint64_t seed = 42;
        if (argc >= 4) {
            const auto parsed = parse_u64(argv[3]);
            if (!parsed) {
                std::cerr << "seed must be an unsigned integer: " << argv[3] << "\n" << kUsage;
                return 2;
            }
            seed = *parsed;
        }
        return generate(std::string(argv[2]), seed);
    }
    if (argc >= 3 && std::string(argv[1]) == "recover") {
        const bool repair = argc >= 4 && std::string(argv[3]) == "--repair";
        if (argc > 4 || (argc == 4 && !repair)) {
            std::cerr << kUsage;
            return 2;
        }
        return recover(std::string(argv[2]), repair);
    }
    if (argc >= 4 && std::string(argv[1]) == "append-loop") {
        if (argc > 5) {
            std::cerr << kUsage;
            return 2;
        }
        const auto mode = parse_mode(argv[3]);
        if (!mode) {
            std::cerr << "mode must be buffered, flush, or fsync: " << argv[3] << "\n" << kUsage;
            return 2;
        }
        std::uint64_t max_records = 0;
        if (argc >= 5) {
            const auto parsed = parse_u64(argv[4]);
            if (!parsed) {
                std::cerr << "max_records must be an unsigned integer: " << argv[4] << "\n"
                          << kUsage;
                return 2;
            }
            max_records = *parsed;
        }
        return append_loop(std::string(argv[2]), *mode, max_records);
    }
    if (argc == 2) {
        return replay(std::string(argv[1]));
    }
    std::cerr << kUsage;
    return 2;
}
