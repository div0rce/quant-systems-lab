#include "qsl/feed/market_data.hpp"
#include "qsl/feed/sequence_tracker.hpp"
#include "qsl/feed/udp_feed.hpp"
#include "qsl/protocol/codec.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

using namespace qsl::feed;

TEST_CASE("sequence tracker reports forward gaps and ignores dup/reorder", "[feed]") {
    SequenceTracker tracker;
    REQUIRE(tracker.observe(1) == 0); // first
    REQUIRE(tracker.observe(2) == 0); // contiguous
    REQUIRE(tracker.observe(4) == 1); // missed 3
    REQUIRE(tracker.observe(5) == 0);
    REQUIRE(tracker.observe(10) == 4); // missed 6,7,8,9
    REQUIRE(tracker.observe(10) == 0); // duplicate
    REQUIRE(tracker.observe(7) == 0);  // out-of-order / old
    REQUIRE(tracker.last() == 10);
}

TEST_CASE("decode_market_data dispatches on message type", "[feed]") {
    const auto trade_bytes = encode(MdTrade{7, 2, 12345, 10});
    const auto trade = decode_market_data(trade_bytes);
    REQUIRE(trade.has_value());
    REQUIRE(std::holds_alternative<MdTrade>(*trade));
    REQUIRE(std::get<MdTrade>(*trade).md_seq == 7);
    REQUIRE(std::get<MdTrade>(*trade).price == 12345);

    const auto tob_bytes = encode(MdTopOfBook{8, 3, std::optional<Price>{100}, std::nullopt});
    const auto tob = decode_market_data(tob_bytes);
    REQUIRE(tob.has_value());
    REQUIRE(std::holds_alternative<MdTopOfBook>(*tob));

    std::array<std::byte, 16> garbage{};
    garbage.fill(static_cast<std::byte>(0xFF));
    REQUIRE_FALSE(decode_market_data(garbage).has_value());
}

TEST_CASE("udp feed delivers market data over a real socket", "[feed][udp]") {
    UdpFeedClient client{0}; // ephemeral port on 127.0.0.1
    REQUIRE(client.good());
    UdpPublisher publisher{"127.0.0.1", client.port()};
    REQUIRE(publisher.good());

    publisher.on_market_data(MdTrade{1, 0, 100, 5});
    publisher.on_market_data(
        MdTopOfBook{2, 0, std::optional<Price>{100}, std::optional<Price>{101}});
    publisher.on_market_data(MdTrade{3, 1, 200, 7});

    std::uint64_t gap = 0;
    const auto first = client.receive(gap);
    REQUIRE(first.has_value());
    REQUIRE(std::holds_alternative<MdTrade>(*first));
    REQUIRE(std::get<MdTrade>(*first).price == 100);
    REQUIRE(gap == 0);

    const auto second = client.receive(gap);
    REQUIRE(second.has_value());
    REQUIRE(std::holds_alternative<MdTopOfBook>(*second));
    REQUIRE(gap == 0);

    const auto third = client.receive(gap);
    REQUIRE(third.has_value());
    REQUIRE(std::get<MdTrade>(*third).md_seq == 3);
    REQUIRE(gap == 0);

    REQUIRE(client.total_gaps() == 0);
}

TEST_CASE("an invalid host leaves the publisher unusable", "[feed][udp]") {
    UdpPublisher publisher{"not-an-ip", 9999};
    REQUIRE_FALSE(publisher.good());
}

TEST_CASE("udp client detects an out-of-sequence gap end-to-end", "[feed][udp]") {
    UdpFeedClient client{0};
    REQUIRE(client.good());
    UdpPublisher publisher{"127.0.0.1", client.port()};
    REQUIRE(publisher.good());

    // Deterministically inject an out-of-sequence stream (md_seq 1 then 3) -- no real loss.
    publisher.on_market_data(MdTrade{1, 0, 100, 5});
    publisher.on_market_data(MdTrade{3, 0, 101, 5});

    std::uint64_t gap = 0;
    const auto first = client.receive(gap);
    REQUIRE(first.has_value());
    REQUIRE(std::get<MdTrade>(*first).md_seq == 1);
    REQUIRE(gap == 0);

    const auto second = client.receive(gap);
    REQUIRE(second.has_value()); // the later message is still accepted
    REQUIRE(std::get<MdTrade>(*second).md_seq == 3);
    REQUIRE(gap == 1); // one message (md_seq 2) missed
    REQUIRE(client.total_gaps() == 1);
}

TEST_CASE("decode_market_data rejects a known non-market-data frame", "[feed]") {
    const auto ack = qsl::protocol::encode(qsl::protocol::Ack{/*order_id=*/1, /*seq=*/2});
    REQUIRE_FALSE(decode_market_data(ack).has_value());
}
