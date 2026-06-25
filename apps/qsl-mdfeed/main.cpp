#include "qsl/engine/matching_engine.hpp"
#include "qsl/feed/market_data.hpp"
#include "qsl/feed/publisher.hpp"
#include "qsl/feed/udp_feed.hpp"
#include "qsl/replay/recovery.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace {

// Parse a whole token as an unsigned integer, rejecting junk/trailing chars/overflow (unlike
// std::stoul/std::stoull which throw on bad input and std::stoi which can wrap a port silently).
std::optional<std::uint64_t> parse_u64(std::string_view s) {
    std::uint64_t value = 0;
    const char *begin = s.data();
    const char *end = begin + s.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::uint16_t> parse_port(std::string_view s) {
    const auto value = parse_u64(s);
    if (!value || *value > std::numeric_limits<std::uint16_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(*value);
}

std::optional<int> parse_rcvbuf(std::string_view s) {
    const auto value = parse_u64(s);
    if (!value || *value > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(*value);
}

template <class... Opts> bool all_present(const Opts &...opts) noexcept {
    return (static_cast<bool>(opts) && ...);
}

int usage() {
    std::cerr << "usage:\n  qsl-mdfeed subscribe <port> [count] [rcvbuf_bytes]\n  qsl-mdfeed "
                 "publish <port> [seed] [orders]\n";
    return 2;
}

int publish(std::uint16_t port, std::uint64_t seed, std::size_t orders) {
    qsl::engine::MatchingEngine engine;
    qsl::feed::MarketDataPublisher publisher;
    qsl::feed::UdpPublisher udp{"127.0.0.1", port};
    if (!udp.good()) {
        std::cerr << "failed to open UDP publisher\n";
        return 1;
    }
    publisher.subscribe(udp);

    // `orders` scales the burst size: more commands -> more datagrams sent back-to-back, which
    // is what stresses the receiver's socket buffer in the M30 burst/gap experiment.
    const auto flow = qsl::replay::generate_flow(seed, /*symbols=*/3, /*orders=*/orders);
    for (const auto &command : flow) {
        const auto events = qsl::replay::apply(engine, command);
        publisher.publish(engine, events);
    }
    std::cout << "published market data (md_seq up to " << publisher.md_seq()
              << ") to 127.0.0.1:" << port << "\n";
    return 0;
}

int subscribe(std::uint16_t port, std::size_t count, int recv_buffer_bytes) {
    qsl::feed::UdpFeedClient client{port, recv_buffer_bytes};
    if (!client.good()) {
        std::cerr << "failed to bind UDP feed client on port " << port << "\n";
        return 1;
    }
    std::cout << "listening for market data on 127.0.0.1:" << client.port()
              << " (SO_RCVBUF=" << client.recv_buffer_bytes() << " bytes)\n";
    std::size_t received = 0;
    int idle_ticks = 0;
    for (std::size_t i = 0; i < count; ++i) {
        std::uint64_t gap = 0;
        const auto msg = client.receive(gap);
        if (!msg) {
            // recv timed out (SO_RCVTIMEO) -> the stream is idle. Stop after a few consecutive
            // idle ticks so a burst experiment terminates promptly even when datagrams were
            // dropped; otherwise it would block one full timeout per missing message.
            if (++idle_ticks >= 3) {
                break;
            }
            continue;
        }
        idle_ticks = 0;
        ++received;
        const std::string flag = (gap > 0) ? "  [GAP " + std::to_string(gap) + "]" : "";
        if (const auto *t = std::get_if<qsl::feed::MdTrade>(&*msg)) {
            std::cout << "trade   seq=" << t->md_seq << " symbol=" << t->symbol
                      << " price=" << t->price << " qty=" << t->quantity << flag << "\n";
        } else if (const auto *tob = std::get_if<qsl::feed::MdTopOfBook>(&*msg)) {
            std::cout << "top     seq=" << tob->md_seq << " symbol=" << tob->symbol << flag << "\n";
        }
    }
    std::cout << "received " << received << " datagrams\n";
    std::cout << "total gaps detected: " << client.total_gaps()
              << " (SO_RCVBUF=" << client.recv_buffer_bytes() << " bytes)\n";
    return 0;
}

} // namespace

int run_subscribe(int argc, char **argv) {
    const auto port = parse_port(argv[2]);
    const auto count = (argc >= 4) ? parse_u64(argv[3]) : std::optional<std::uint64_t>{20};
    const auto rcvbuf = (argc >= 5) ? parse_rcvbuf(argv[4]) : std::optional<int>{0};
    if (!all_present(port, count, rcvbuf)) {
        return usage();
    }
    return subscribe(*port, static_cast<std::size_t>(*count), *rcvbuf);
}

int run_publish(int argc, char **argv) {
    const auto port = parse_port(argv[2]);
    const auto seed = (argc >= 4) ? parse_u64(argv[3]) : std::optional<std::uint64_t>{42};
    const auto orders = (argc >= 5) ? parse_u64(argv[4]) : std::optional<std::uint64_t>{200};
    if (!all_present(port, seed, orders)) {
        return usage();
    }
    return publish(*port, *seed, static_cast<std::size_t>(*orders));
}

// qsl-mdfeed subscribe <port> [count] [rcvbuf_bytes]  -> receive datagrams and report gaps
// qsl-mdfeed publish   <port> [seed] [orders]         -> run a synthetic flow, stream its data
int main(int argc, char **argv) {
    if (argc >= 3 && std::string(argv[1]) == "subscribe") {
        return run_subscribe(argc, argv);
    }
    if (argc >= 3 && std::string(argv[1]) == "publish") {
        return run_publish(argc, argv);
    }
    return usage();
}
