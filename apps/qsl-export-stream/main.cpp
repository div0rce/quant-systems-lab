#include "qsl/replay/fixture.hpp"

#include <cstdint>
#include <iostream>
#include <string>

// qsl-export-stream [seed] [orders]
//
// Writes a normalized differential-testing fixture (command stream + engine events +
// rejections + final per-symbol snapshot) to stdout. Deterministic for a given seed; this
// is the data the independent OCaml replay engine (M16) consumes and M17 compares against.
int main(int argc, char **argv) {
    if (argc >= 2 && std::string(argv[1]) == "ioc") {
        qsl::replay::write_ioc_scenario_fixture(std::cout);
        return 0;
    }
    if (argc >= 3 && std::string(argv[1]) == "prop") {
        qsl::replay::write_property_fixture(std::cout, std::stoull(argv[2]));
        return 0;
    }
    qsl::replay::FixtureParams params;
    if (argc >= 2) {
        params.seed = std::stoull(argv[1]);
    }
    if (argc >= 3) {
        params.orders = std::stoull(argv[2]);
    }
    qsl::replay::write_stream_fixture(std::cout, params);
    return 0;
}
