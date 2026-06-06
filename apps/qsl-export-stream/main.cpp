#include "qsl/replay/fixture.hpp"

#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
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

qsl::replay::FixtureExportRequest parse_request(int argc, char **argv) {
    qsl::replay::FixtureExportRequest request;
    if (argc >= 2) {
        const std::string mode = argv[1];
        if (mode == "version") {
            request.mode = qsl::replay::FixtureExportMode::Version;
            return request;
        }
        if (mode == "ioc") {
            request.mode = qsl::replay::FixtureExportMode::IocScenario;
            return request;
        }
        if (mode == "prop") {
            if (argc < 3) {
                throw std::invalid_argument("prop requires <seed>");
            }
            request.mode = qsl::replay::FixtureExportMode::Property;
            request.seed = parse_u64(argv[2], "prop seed");
            return request;
        }
        if (mode == "shrink") {
            if (argc < 3) {
                throw std::invalid_argument("shrink requires <seed>");
            }
            request.mode = qsl::replay::FixtureExportMode::Shrink;
            request.seed = parse_u64(argv[2], "shrink seed");
            return request;
        }
        if (mode == "divergence") {
            if (argc < 3) {
                throw std::invalid_argument("divergence requires <seed>");
            }
            request.mode = qsl::replay::FixtureExportMode::Divergence;
            request.seed = parse_u64(argv[2], "divergence seed");
            return request;
        }
        request.params.seed = parse_u64(argv[1], "seed");
    }
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
