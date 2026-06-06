#include "qsl/replay/fixture.hpp"

#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

constexpr std::string_view kUsage =
    "usage: qsl-export-stream [seed] [orders] | version | ioc | prop <seed> | shrink <seed> | "
    "divergence <seed>";

std::uint64_t parse_u64(std::string_view value, std::string_view name) {
    std::uint64_t parsed = 0;
    const auto *begin = value.data();
    const auto *end = begin + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec == std::errc::result_out_of_range) {
        throw std::out_of_range(std::string(name) + " is out of range: " + std::string(value));
    }
    if (ec != std::errc{} || ptr != end) {
        throw std::invalid_argument(std::string(name) +
                                    " must be an unsigned integer: " + std::string(value));
    }
    return parsed;
}

qsl::replay::FixtureExportRequest mode_only_request(qsl::replay::FixtureExportMode mode) {
    qsl::replay::FixtureExportRequest request;
    request.mode = mode;
    return request;
}

std::string_view required_seed_arg(int argc, char **argv, std::string_view mode) {
    if (argc < 3) {
        throw std::invalid_argument(std::string(mode) + " requires <seed>");
    }
    return argv[2];
}

qsl::replay::FixtureExportRequest seeded_mode_request(qsl::replay::FixtureExportMode mode,
                                                      std::string_view seed,
                                                      std::string_view name) {
    qsl::replay::FixtureExportRequest request;
    request.mode = mode;
    request.seed = parse_u64(seed, name);
    return request;
}

std::optional<qsl::replay::FixtureExportRequest> parse_named_mode(std::string_view mode, int argc,
                                                                  char **argv) {
    using qsl::replay::FixtureExportMode;
    if (mode == "version") {
        return mode_only_request(FixtureExportMode::Version);
    }
    if (mode == "ioc") {
        return mode_only_request(FixtureExportMode::IocScenario);
    }
    if (mode == "prop") {
        return seeded_mode_request(FixtureExportMode::Property, required_seed_arg(argc, argv, mode),
                                   "prop seed");
    }
    if (mode == "shrink") {
        return seeded_mode_request(FixtureExportMode::Shrink, required_seed_arg(argc, argv, mode),
                                   "shrink seed");
    }
    if (mode == "divergence") {
        return seeded_mode_request(FixtureExportMode::Divergence,
                                   required_seed_arg(argc, argv, mode), "divergence seed");
    }
    return std::nullopt;
}

qsl::replay::FixtureExportRequest parse_request(int argc, char **argv) {
    qsl::replay::FixtureExportRequest request;
    if (argc < 2) {
        return request;
    }
    if (const auto named = parse_named_mode(argv[1], argc, argv)) {
        return *named;
    }
    request.params.seed = parse_u64(argv[1], "seed");
    if (argc >= 3) {
        request.params.orders = parse_u64(argv[2], "orders");
    }
    return request;
}

} // namespace

// qsl-export-stream [seed] [orders]
//
// Writes a normalized differential-testing fixture (command stream + engine events +
// rejections + final per-symbol snapshot) to stdout. Deterministic for a given seed; this
// is the data the independent OCaml replay engine (M16) consumes and M17 compares against.
int main(int argc, char **argv) {
    try {
        qsl::replay::write_fixture_export(std::cout, parse_request(argc, argv));
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "error: " << ex.what() << "\n" << kUsage << "\n";
        return 2;
    }
}
