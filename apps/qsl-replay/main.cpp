#include "qsl/engine/matching_engine.hpp"
#include "qsl/replay/command.hpp"
#include "qsl/replay/event_log.hpp"
#include "qsl/replay/recovery.hpp"

#include <cstdint>
#include <iostream>
#include <string>

namespace {

int generate(const std::string &path, std::uint64_t seed) {
    const auto flow = qsl::replay::generate_flow(seed, /*symbols=*/4, /*orders=*/500);
    qsl::replay::EventLogWriter writer{path};
    if (!writer.good()) {
        std::cerr << "cannot open " << path << " for writing\n";
        return 1;
    }
    std::uint64_t seq = 0;
    for (const auto &command : flow) {
        writer.append(qsl::replay::LogRecord{seq, qsl::replay::RecordType::Command, seq,
                                             qsl::replay::encode_command(command)});
        ++seq;
    }
    std::cout << "wrote " << flow.size() << " command records to " << path << "\n";
    return 0;
}

int replay(const std::string &path) {
    const qsl::replay::EventLogReader reader{path};
    const auto log = reader.read_all();
    if (log.error != qsl::replay::LogError::None) {
        std::cerr << "log read error\n";
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

// qsl-replay generate <file> [seed]   -> write a deterministic synthetic-flow command log
// qsl-replay <file>                   -> rebuild engine state from the log and print it
int main(int argc, char **argv) {
    if (argc >= 3 && std::string(argv[1]) == "generate") {
        const std::uint64_t seed = (argc >= 4) ? std::stoull(argv[3]) : 42;
        return generate(std::string(argv[2]), seed);
    }
    if (argc == 2) {
        return replay(std::string(argv[1]));
    }
    std::cerr << "usage:\n  qsl-replay generate <file> [seed]\n  qsl-replay <file>\n";
    return 2;
}
