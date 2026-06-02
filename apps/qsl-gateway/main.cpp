#include "qsl/engine/matching_engine.hpp"
#include "qsl/gateway/epoll_server.hpp"
#include "qsl/gateway/order_gateway.hpp"
#include "qsl/gateway/tcp_server.hpp"

#include <cstdint>
#include <iostream>
#include <string>

// qsl-gateway [port] [--epoll] -> serve on 127.0.0.1:port (default 9009).
// No authentication; localhost only. This is a local simulator, not a real venue.
int main(int argc, char **argv) {
    const std::uint16_t port = (argc >= 2) ? static_cast<std::uint16_t>(std::stoul(argv[1])) : 9009;
    const bool use_epoll = argc >= 3 && std::string(argv[2]) == "--epoll";

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
