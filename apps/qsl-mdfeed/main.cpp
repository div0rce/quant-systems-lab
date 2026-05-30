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

int publish(std::uint16_t port, std::uint64_t seed) {
    qsl::engine::MatchingEngine engine;
    qsl::feed::MarketDataPublisher publisher;
    qsl::feed::UdpPublisher udp{"127.0.0.1", port};
    if (!udp.good()) {
        std::cerr << "failed to open UDP publisher\n";
        return 1;
    }
    publisher.subscribe(udp);

    const auto flow = qsl::replay::generate_flow(seed, /*symbols=*/3, /*orders=*/200);
    for (const auto &command : flow) {
        const auto events = qsl::replay::apply(engine, command);
        publisher.publish(engine, events);
    }
    std::cout << "published market data (md_seq up to " << publisher.md_seq()
              << ") to 127.0.0.1:" << port << "\n";
    return 0;
}

int subscribe(std::uint16_t port, std::size_t count) {
    qsl::feed::UdpFeedClient client{port};
    if (!client.good()) {
        std::cerr << "failed to bind UDP feed client on port " << port << "\n";
        return 1;
    }
    std::cout << "listening for market data on 127.0.0.1:" << client.port() << "\n";
    for (std::size_t i = 0; i < count; ++i) {
        std::uint64_t gap = 0;
        const auto msg = client.receive(gap);
        if (!msg) {
            continue;
        }
        const std::string flag = (gap > 0) ? "  [GAP " + std::to_string(gap) + "]" : "";
        if (const auto *t = std::get_if<qsl::feed::MdTrade>(&*msg)) {
            std::cout << "trade   seq=" << t->md_seq << " symbol=" << t->symbol
                      << " price=" << t->price << " qty=" << t->quantity << flag << "\n";
        } else if (const auto *tob = std::get_if<qsl::feed::MdTopOfBook>(&*msg)) {
            std::cout << "top     seq=" << tob->md_seq << " symbol=" << tob->symbol << flag << "\n";
        }
    }
    std::cout << "total gaps detected: " << client.total_gaps() << "\n";
    return 0;
}

} // namespace

// qsl-mdfeed subscribe <port> [count]  -> receive market-data datagrams and report gaps
// qsl-mdfeed publish   <port> [seed]   -> run a synthetic flow and stream its market data
int main(int argc, char **argv) {
    if (argc >= 3 && std::string(argv[1]) == "subscribe") {
        const auto port = static_cast<std::uint16_t>(std::stoul(argv[2]));
        const std::size_t count = (argc >= 4) ? std::stoul(argv[3]) : 20;
        return subscribe(port, count);
    }
    if (argc >= 3 && std::string(argv[1]) == "publish") {
        const auto port = static_cast<std::uint16_t>(std::stoul(argv[2]));
        const std::uint64_t seed = (argc >= 4) ? std::stoull(argv[3]) : 42;
        return publish(port, seed);
    }
    std::cerr
        << "usage:\n  qsl-mdfeed subscribe <port> [count]\n  qsl-mdfeed publish <port> [seed]\n";
    return 2;
}
