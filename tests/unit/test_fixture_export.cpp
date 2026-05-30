#include "qsl/replay/fixture.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <iterator>
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

std::vector<std::string> lines_of(const std::string &s) {
    std::vector<std::string> lines;
    std::istringstream is(s);
    std::string line;
    while (std::getline(is, line)) {
        lines.push_back(line);
    }
    return lines;
}

std::size_t find_line(const std::vector<std::string> &lines, const std::string &needle) {
    const auto it = std::find(lines.begin(), lines.end(), needle);
    REQUIRE(it != lines.end());
    return static_cast<std::size_t>(std::distance(lines.begin(), it));
}

bool contains_line(const std::vector<std::string> &lines, const std::string &needle) {
    return std::find(lines.begin(), lines.end(), needle) != lines.end();
}

bool contains_prefix(const std::vector<std::string> &lines, const std::string &prefix) {
    return std::any_of(lines.begin(), lines.end(),
                       [&](const std::string &line) { return line.rfind(prefix, 0) == 0; });
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
    std::uint64_t meta_max_qty = 0;
    std::uint64_t meta_max_notional = 0;
    bool have_version = false, have_snapshot = false, have_meta_max_qty = false,
         have_meta_max_notional = false;
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
            needs(t, 11);
            REQUIRE(t[1] == "seed");
            REQUIRE(t[3] == "symbols");
            REQUIRE(t[5] == "orders");
            REQUIRE(t[7] == "max_qty");
            REQUIRE(t[9] == "max_notional");
            meta_max_qty = std::stoull(t[8]);
            meta_max_notional = std::stoull(t[10]);
            have_meta_max_qty = true;
            have_meta_max_notional = true;
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
            needs(t, 4);
            const std::string &kind = t[1];
            REQUIRE((kind == "new_limit" || kind == "new_market" || kind == "cancel" ||
                     kind == "modify"));
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
    REQUIRE(have_meta_max_qty);
    REQUIRE(have_meta_max_notional);
    REQUIRE(have_snapshot);
    REQUIRE(meta_max_qty == 8);
    REQUIRE(meta_max_notional == 1'000'000);
    REQUIRE(snap_last_seq == max_seq);  // snapshot last_seq ties to the event stream
    REQUIRE(snap_trades == trade_evts); // reported trade count matches emitted trades
    REQUIRE(cmds > 0);                  // non-vacuous: real commands, trades, and rejects
    REQUIRE(trade_evts > 0);
    REQUIRE(rejects > 0);
}

TEST_CASE("differential fixture emits structured modify rejection outcomes", "[fixture]") {
    const auto lines = lines_of(make_fixture(7, 60));

    const std::size_t idx = find_line(lines, "cmd modify 3 7 95 9");
    REQUIRE(idx + 1 < lines.size());
    REQUIRE(lines[idx + 1] == "reject modify 7 MaxQuantityExceeded");
}

TEST_CASE("rejected modify emits no engine event and does not mutate snapshot", "[fixture]") {
    const auto lines = lines_of(make_fixture(7, 60));

    const std::size_t idx = find_line(lines, "cmd modify 3 7 95 9");
    REQUIRE(idx + 1 < lines.size());
    REQUIRE(lines[idx + 1] == "reject modify 7 MaxQuantityExceeded");

    for (std::size_t i = idx + 2;
         i < lines.size() && lines[i].rfind("cmd ", 0) != 0 && lines[i].rfind("snapshot ", 0) != 0;
         ++i) {
        REQUIRE(lines[i].rfind("evt ", 0) != 0);
    }

    const std::size_t next_event = find_line(lines, "evt accept 9 3 8");
    REQUIRE(next_event > idx);
    REQUIRE(contains_line(lines, "level 3 B 98 8"));
    REQUIRE_FALSE(contains_line(lines, "level 3 B 95 9"));
}

TEST_CASE("registered but empty symbols are exported in the snapshot", "[fixture]") {
    qsl::replay::FixtureParams params;
    params.seed = 7;
    params.symbols = 1;
    params.orders = 0;

    std::ostringstream os;
    qsl::replay::write_stream_fixture(os, params);
    const auto lines = lines_of(os.str());

    REQUIRE(contains_line(lines, "cmd reg S0"));
    REQUIRE(contains_line(lines, "sym 0 bid - ask - orders 0"));
    REQUIRE_FALSE(contains_prefix(lines, "level 0 "));
}
