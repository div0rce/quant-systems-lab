#include "qsl/replay/event_log.hpp"

#include <cstdint>
#include <iostream>
#include <string>

namespace {
const char *to_string(qsl::replay::LogError e) noexcept {
    switch (e) {
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
} // namespace

// Human-inspectable summary of an append-only event log.
int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: qsl-loginspect <log-file>\n";
        return 2;
    }

    const qsl::replay::EventLogReader reader{std::string(argv[1])};
    const auto result = reader.read_all();

    std::uint64_t commands = 0;
    std::uint64_t events = 0;
    for (const auto &record : result.records) {
        if (record.type == qsl::replay::RecordType::CommandRecord) {
            ++commands;
        } else if (record.type == qsl::replay::RecordType::EventRecord) {
            ++events;
        }
    }

    std::cout << "records: " << result.records.size() << "\n";
    if (!result.records.empty()) {
        std::cout << "seq range: " << result.records.front().seq_no << ".."
                  << result.records.back().seq_no << "\n";
    }
    std::cout << "commands: " << commands << "  events: " << events << "\n";
    std::cout << "status: " << to_string(result.error) << "\n";

    return result.error == qsl::replay::LogError::None ? 0 : 1;
}
