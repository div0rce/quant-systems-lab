#include "qsl/engine/matching_engine.hpp"
#include "qsl/feed/market_data.hpp"
#include "qsl/feed/publisher.hpp"
#include "qsl/feed/udp_feed.hpp"
#include "qsl/replay/recovery.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <variant>

namespace {

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

// qsl-mdfeed subscribe <port> [count] [rcvbuf_bytes]  -> receive datagrams and report gaps
// qsl-mdfeed publish   <port> [seed] [orders]         -> run a synthetic flow, stream its data
int main(int argc, char **argv) {
    if (argc >= 3 && std::string(argv[1]) == "subscribe") {
        const auto port = static_cast<std::uint16_t>(std::stoul(argv[2]));
        const std::size_t count = (argc >= 4) ? std::stoul(argv[3]) : 20;
        const int recv_buffer_bytes = (argc >= 5) ? std::stoi(argv[4]) : 0;
        return subscribe(port, count, recv_buffer_bytes);
    }
    if (argc >= 3 && std::string(argv[1]) == "publish") {
        const auto port = static_cast<std::uint16_t>(std::stoul(argv[2]));
        const std::uint64_t seed = (argc >= 4) ? std::stoull(argv[3]) : 42;
        const std::size_t orders = (argc >= 5) ? std::stoul(argv[4]) : 200;
        return publish(port, seed, orders);
    }
    std::cerr << "usage:\n  qsl-mdfeed subscribe <port> [count] [rcvbuf_bytes]\n  qsl-mdfeed "
                 "publish <port> [seed] [orders]\n";
    return 2;
}
