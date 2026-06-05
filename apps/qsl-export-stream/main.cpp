#include "qsl/replay/fixture.hpp"

#include <cstdint>
#include <iostream>
#include <string>

namespace {

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
        if (argc >= 3 && mode == "prop") {
            request.mode = qsl::replay::FixtureExportMode::Property;
            request.seed = std::stoull(argv[2]);
            return request;
        }
        if (argc >= 3 && mode == "shrink") {
            request.mode = qsl::replay::FixtureExportMode::Shrink;
            request.seed = std::stoull(argv[2]);
            return request;
        }
        if (argc >= 3 && mode == "divergence") {
            request.mode = qsl::replay::FixtureExportMode::Divergence;
            request.seed = std::stoull(argv[2]);
            return request;
        }
        request.params.seed = std::stoull(argv[1]);
    }
    if (argc >= 3) {
        request.params.orders = std::stoull(argv[2]);
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
    qsl::replay::write_fixture_export(std::cout, parse_request(argc, argv));
    return 0;
}
