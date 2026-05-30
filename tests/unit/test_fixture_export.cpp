#include "qsl/replay/fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::vector<std::string> split(const std::string &s) {
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string tok;
    while (is >> tok) {
        out.push_back(tok);
    }
    return out;
}

std::string make_fixture(std::uint64_t seed, std::size_t orders) {
    qsl::replay::FixtureParams params;
    params.seed = seed;
    params.orders = orders;
    std::ostringstream os;
    qsl::replay::write_stream_fixture(os, params);
    return os.str();
}

} // namespace

TEST_CASE("differential fixture export is deterministic", "[fixture]") {
    REQUIRE(make_fixture(7, 60) == make_fixture(7, 60)); // same seed -> identical bytes
    REQUIRE(make_fixture(7, 60) != make_fixture(8, 60)); // different seed -> different output
}

// Validate the documented grammar line-by-line and cross-check the snapshot against the
// event stream. Proves the exported fixture is parseable and internally consistent.
TEST_CASE("differential fixture is well-formed and self-consistent", "[fixture]") {
    const std::string fixture = make_fixture(7, 60);
    std::istringstream is(fixture);
    std::string line;

    long cmds = 0, trade_evts = 0, rejects = 0, max_seq = 0;
    long snap_last_seq = -1, snap_trades = -1;
    bool have_version = false, have_snapshot = false;
    const auto needs = [](const std::vector<std::string> &t, std::size_t n) {
        REQUIRE(t.size() == n);
    };

    while (std::getline(is, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto t = split(line);
        REQUIRE_FALSE(t.empty());
        const std::string &k = t[0];
        if (k == "version") {
            needs(t, 2);
            REQUIRE(std::stoi(t[1]) == 1);
            have_version = true;
        } else if (k == "meta") {
            REQUIRE(t.size() >= 2);
        } else if (k == "cmd") {
            ++cmds;
            REQUIRE(t.size() >= 2);
            const std::string &kind = t[1];
            if (kind == "reg") {
                needs(t, 3);
            } else if (kind == "limit") {
                needs(t, 8);
            } else if (kind == "market") {
                needs(t, 6);
            } else if (kind == "cancel") {
                needs(t, 4);
            } else if (kind == "modify") {
                needs(t, 6);
            } else {
                FAIL("unknown cmd kind: " + kind);
            }
        } else if (k == "evt") {
            REQUIRE(t.size() >= 3);
            const std::string &kind = t[1];
            const long seq = std::stol(t[2]);
            if (seq > max_seq) {
                max_seq = seq;
            }
            if (kind == "accept" || kind == "cancel" || kind == "modify") {
                needs(t, 5);
            } else if (kind == "trade") {
                needs(t, 8);
                ++trade_evts;
            } else {
                FAIL("unknown evt kind: " + kind);
            }
        } else if (k == "reject") {
            needs(t, 3);
            ++rejects;
        } else if (k == "snapshot") {
            needs(t, 5);
            REQUIRE(t[1] == "last_seq");
            REQUIRE(t[3] == "trades");
            snap_last_seq = std::stol(t[2]);
            snap_trades = std::stol(t[4]);
            have_snapshot = true;
        } else if (k == "sym") {
            needs(t, 8);
        } else if (k == "level") {
            needs(t, 5);
        } else {
            FAIL("unknown record: " + k);
        }
    }

    REQUIRE(have_version);
    REQUIRE(have_snapshot);
    REQUIRE(snap_last_seq == max_seq);  // snapshot last_seq ties to the event stream
    REQUIRE(snap_trades == trade_evts); // reported trade count matches emitted trades
    REQUIRE(cmds > 0);                  // non-vacuous: real commands, trades, and rejects
    REQUIRE(trade_evts > 0);
    REQUIRE(rejects > 0);
}
