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

struct RawValue {
    std::string_view text;
};

struct LogPath {
    std::string value;
};

struct Seed {
    std::uint64_t value;
};

struct MaxRecords {
    std::uint64_t value;
};

struct RepairFlag {
    bool enabled;
};

struct GenerateRequest {
    LogPath path;
    Seed seed;
};

struct RecoverRequest {
    LogPath path;
    RepairFlag repair;
};

struct AppendLoopRequest {
    LogPath path;
    qsl::replay::DurabilityMode mode;
    MaxRecords max_records;
};

struct MainArgs {
    int count;
    char **values;
};

struct CommandLine {
    std::vector<std::string_view> values;

    [[nodiscard]] std::size_t size() const noexcept { return values.size(); }
    [[nodiscard]] RawValue command_name() const noexcept { return {values[1]}; }
    [[nodiscard]] LogPath log_path() const { return {std::string(values[2])}; }
    [[nodiscard]] RawValue seed_arg() const noexcept { return {values[3]}; }
    [[nodiscard]] RawValue recover_option() const noexcept { return {values[3]}; }
    [[nodiscard]] RawValue mode_arg() const noexcept { return {values[3]}; }
    [[nodiscard]] RawValue max_records_arg() const noexcept { return {values[4]}; }
};

CommandLine make_command_line(MainArgs args) {
    CommandLine line;
    line.values.reserve(static_cast<std::size_t>(args.count));
    for (int i = 0; i < args.count; ++i) {
        line.values.emplace_back(args.values[i]);
    }
    return line;
}

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

std::optional<std::uint64_t> parse_u64(RawValue value) {
    std::uint64_t parsed = 0;
    const auto *begin = value.text.data();
    const auto *end = begin + value.text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<qsl::replay::DurabilityMode> parse_mode(RawValue value) {
    if (value.text == "buffered") {
        return qsl::replay::DurabilityMode::BufferedOnly;
    }
    if (value.text == "flush") {
        return qsl::replay::DurabilityMode::FlushOnAppend;
    }
    if (value.text == "fsync") {
        return qsl::replay::DurabilityMode::FsyncOnAppend;
    }
    return std::nullopt;
}

enum class NumericArg : std::uint8_t {
    Seed,
    MaxRecords,
};

enum class CommandName : std::uint8_t {
    Generate,
    Recover,
    AppendLoop,
};

const char *to_string(NumericArg arg) noexcept {
    switch (arg) {
    case NumericArg::Seed:
        return "seed";
    case NumericArg::MaxRecords:
        return "max_records";
    }
    return "value";
}

std::string_view spelling(CommandName name) noexcept {
    switch (name) {
    case CommandName::Generate:
        return "generate";
    case CommandName::Recover:
        return "recover";
    case CommandName::AppendLoop:
        return "append-loop";
    }
    return {};
}

bool command_is(const CommandLine &line, CommandName name) {
    return line.command_name().text == spelling(name);
}

int usage_error() {
    std::cerr << kUsage;
    return 2;
}

int unsigned_arg_error(NumericArg arg, RawValue value) {
    std::cerr << to_string(arg) << " must be an unsigned integer: " << value.text << "\n" << kUsage;
    return 2;
}

int generate(GenerateRequest request) {
    const auto flow = qsl::replay::generate_flow(request.seed.value, /*symbols=*/4, /*orders=*/500);
    qsl::replay::EventLogWriter writer{request.path.value};
    if (!writer.good()) {
        std::cerr << "cannot open " << request.path.value << " for writing\n";
        return 1;
    }
    std::uint64_t seq = 0;
    for (const auto &command : flow) {
        if (!writer.append(qsl::replay::LogRecord{seq, qsl::replay::RecordType::CommandRecord, seq,
                                                  qsl::replay::encode_command(command)})) {
            std::cerr << "append failed for " << request.path.value << " at record " << seq << "\n";
            return 1;
        }
        ++seq;
    }
    std::cout << "wrote " << flow.size() << " command records to " << request.path.value << "\n";
    return 0;
}

int run_generate_command(const CommandLine &line) {
    Seed seed{42};
    if (line.size() >= 4) {
        const auto parsed = parse_u64(line.seed_arg());
        if (!parsed) {
            return unsigned_arg_error(NumericArg::Seed, line.seed_arg());
        }
        seed.value = *parsed;
    }
    return generate({line.log_path(), seed});
}

// Crash-recovery inspection: report the longest valid prefix and the tail classification,
// optionally truncating a torn tail back to the last valid record boundary. Exit 0 means the
// log is clean after the operation; corrupt logs are never repaired by automation.
int recover(RecoverRequest request) {
    const auto recovery = qsl::replay::recover_log_file(request.path.value);
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
    if (!request.repair.enabled) {
        std::cerr << "torn tail found; rerun with --repair to truncate it\n";
        return 1;
    }
    if (!qsl::replay::repair_log_file(request.path.value, recovery)) {
        std::cerr << "repair failed\n";
        return 1;
    }
    std::cout << "repaired: truncated to " << recovery.valid_bytes << " bytes\n";
    return 0;
}

std::optional<RepairFlag> parse_repair_arg(const CommandLine &line) {
    if (line.size() == 3) {
        return RepairFlag{false};
    }
    if (line.size() != 4) {
        return std::nullopt;
    }
    if (line.recover_option().text != "--repair") {
        return std::nullopt;
    }
    return RepairFlag{true};
}

int run_recover_command(const CommandLine &line) {
    const auto repair = parse_repair_arg(line);
    if (!repair) {
        return usage_error();
    }
    return recover({line.log_path(), *repair});
}

// Append deterministic records until killed (or until max_records when nonzero), printing an
// `ack <seq>` line after each append the writer reported as complete. A crash harness can
// SIGKILL this process mid-stream and compare the acknowledged count against what recovery
// finds in the file.
int append_loop(AppendLoopRequest request) {
    qsl::replay::EventLogWriter writer{request.path.value, request.mode};
    if (!writer.good()) {
        std::cerr << "cannot open " << request.path.value << " for writing\n";
        return 1;
    }
    for (std::uint64_t seq = 1; request.max_records.value == 0 || seq <= request.max_records.value;
         ++seq) {
        std::vector<std::byte> payload(16);
        qsl::protocol::store_be<std::uint64_t>(payload.data(), seq);
        qsl::protocol::store_be<std::uint64_t>(payload.data() + 8, seq ^ 0xA5A5A5A5A5A5A5A5ULL);
        if (!writer.append(qsl::replay::LogRecord{seq, qsl::replay::RecordType::CommandRecord, seq,
                                                  std::move(payload)})) {
            std::cerr << "append failed for " << request.path.value << " at record " << seq << "\n";
            return 1;
        }
        std::cout << "ack " << seq << "\n" << std::flush;
    }
    return 0;
}

int run_append_loop_command(const CommandLine &line) {
    if (line.size() > 5) {
        return usage_error();
    }
    const auto mode = parse_mode(line.mode_arg());
    if (!mode) {
        std::cerr << "mode must be buffered, flush, or fsync: " << line.mode_arg().text << "\n"
                  << kUsage;
        return 2;
    }
    MaxRecords max_records{0};
    if (line.size() >= 5) {
        const auto parsed = parse_u64(line.max_records_arg());
        if (!parsed) {
            return unsigned_arg_error(NumericArg::MaxRecords, line.max_records_arg());
        }
        max_records.value = *parsed;
    }
    return append_loop({line.log_path(), *mode, max_records});
}

int replay(LogPath path) {
    const qsl::replay::EventLogReader reader{path.value};
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

int dispatch(const CommandLine &line) {
    if (line.size() >= 3 && command_is(line, CommandName::Generate)) {
        return run_generate_command(line);
    }
    if (line.size() >= 3 && command_is(line, CommandName::Recover)) {
        return run_recover_command(line);
    }
    if (line.size() >= 4 && command_is(line, CommandName::AppendLoop)) {
        return run_append_loop_command(line);
    }
    if (line.size() == 2) {
        return replay({std::string(line.values[1])});
    }
    return usage_error();
}

} // namespace

// qsl-replay generate <file> [seed]                        -> write a synthetic-flow command log
// qsl-replay recover <file> [--repair]                     -> classify and optionally repair a log
// qsl-replay append-loop <file> <mode> [max_records]       -> append until killed (crash harness)
// qsl-replay <file>                                        -> rebuild engine state from the log
int main(int argc, char **argv) {
    return dispatch(make_command_line({argc, argv}));
}
