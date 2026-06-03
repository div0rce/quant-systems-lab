#include "qsl/engine/matching_engine.hpp"
#include "qsl/gateway/epoll_server.hpp"
#include "qsl/gateway/order_gateway.hpp"
#include "qsl/gateway/tcp_server.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <string>

// qsl-gateway [port] [--epoll] -> serve on 127.0.0.1:port (default 9009).
// No authentication; localhost only. This is a local simulator, not a real venue.
int main(int argc, char **argv) {
    // Flags may appear before or instead of the port; the first non-flag argument is the port. So
    // `qsl-gateway`, `qsl-gateway 9009`, `qsl-gateway --epoll`, and `qsl-gateway 9009 --epoll` all
    // work (parsing `--epoll` as a port previously aborted with std::invalid_argument).
    std::uint16_t port = 9009;
    bool use_epoll = false;
    bool port_set = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--epoll") {
            use_epoll = true;
            continue;
        }
        // The first non-flag arg is the port. Require the WHOLE token to be a single in-range
        // number, so typos like "9009x" (std::stoul stops at the 'x' and accepts 9009), out-of-
        // range values like "70000" (the uint16_t cast would truncate to 4464), or a second port
        // token like "9009 9010" (which would silently bind the last) all fail fast.
        std::size_t consumed = 0;
        unsigned long value = 0;
        try {
            value = std::stoul(arg, &consumed);
        } catch (const std::exception &) {
            consumed = 0; // parse failed -> fall through to the usage error
        }
        if (port_set || consumed != arg.size() ||
            value > std::numeric_limits<std::uint16_t>::max()) {
            std::cerr << "usage: qsl-gateway [port] [--epoll]   (one optional port, 0-65535)\n";
            return 2;
        }
        port = static_cast<std::uint16_t>(value);
        port_set = true;
    }

    qsl::engine::MatchingEngine engine;
    engine.register_symbol("AAPL"); // SymbolId 0
    engine.register_symbol("MSFT"); // SymbolId 1
    qsl::gateway::OrderGateway gateway{engine,
                                       qsl::gateway::RiskConfig{/*max_order_quantity=*/1'000'000,
                                                                /*max_notional=*/1'000'000'000}};
    if (use_epoll) {
        qsl::gateway::EpollServer server{gateway};
        if (!qsl::gateway::EpollServer::supported()) {
            std::cerr << "epoll gateway mode requires Linux\n";
            return 2;
        }

        std::cout << "qsl-gateway epoll listening on 127.0.0.1:" << port
                  << " (no auth, localhost only)\n";
        if (!server.run("127.0.0.1", port)) {
            std::cerr << "failed to start epoll server on port " << port << "\n";
            return 1;
        }
        return 0;
    }

    qsl::gateway::TcpServer server{gateway};

    std::cout << "qsl-gateway listening on 127.0.0.1:" << port << " (no auth, localhost only)\n";
    if (!server.run("127.0.0.1", port)) {
        std::cerr << "failed to start server on port " << port << "\n";
        return 1;
    }
    return 0;
}
