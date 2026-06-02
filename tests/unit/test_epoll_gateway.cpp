#include "qsl/engine/matching_engine.hpp"
#include "qsl/gateway/epoll_server.hpp"
#include "qsl/gateway/order_gateway.hpp"
#include "qsl/protocol/codec.hpp"

#include <catch2/catch_test_macros.hpp>

#if defined(__linux__)
#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <netinet/in.h>
#include <span>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>
#endif

using namespace qsl::gateway;
using namespace qsl::protocol;

#if defined(__linux__)
namespace {

void set_read_timeout(int fd) {
    timeval timeout{};
    timeout.tv_sec = 2;
    REQUIRE(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                         static_cast<socklen_t>(sizeof(timeout))) == 0);
}

void write_all(int fd, const std::vector<std::byte> &data) {
    std::size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        REQUIRE(n > 0);
        off += static_cast<std::size_t>(n);
    }
}

std::vector<MsgType> read_types(int fd, std::size_t expected) {
    std::vector<std::byte> received;
    std::vector<MsgType> types;
    std::array<std::byte, 4096> buf{};

    while (types.size() < expected) {
        const ssize_t n = ::read(fd, buf.data(), buf.size());
        REQUIRE(n > 0);
        received.insert(received.end(), buf.begin(), buf.begin() + n);

        std::size_t off = 0;
        while (off + kHeaderSize <= received.size()) {
            const auto header = decode_header(std::span<const std::byte>(received).subspan(off));
            REQUIRE(header.error == DecodeError::None);
            const std::size_t total = kHeaderSize + header.value.body_len;
            if (off + total > received.size()) {
                break;
            }
            types.push_back(header.value.type);
            off += total;
        }
        received.erase(received.begin(), received.begin() + static_cast<std::ptrdiff_t>(off));
    }
    return types;
}

int bind_loopback_listener(sockaddr_in &bound) {
    const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listen_fd >= 0);
    int yes = 1;
    REQUIRE(::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes,
                         static_cast<socklen_t>(sizeof(yes))) == 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    REQUIRE(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
    auto *addr_generic = reinterpret_cast<sockaddr *>(&addr); // NOLINT: POSIX socket API
    REQUIRE(::bind(listen_fd, addr_generic, static_cast<socklen_t>(sizeof(addr))) == 0);
    REQUIRE(::listen(listen_fd, 16) == 0);

    socklen_t bound_len = sizeof(bound);
    auto *bound_generic = reinterpret_cast<sockaddr *>(&bound); // NOLINT: POSIX socket API
    REQUIRE(::getsockname(listen_fd, bound_generic, &bound_len) == 0);
    return listen_fd;
}

int connect_client(const sockaddr_in &bound) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    set_read_timeout(fd);
    const auto *generic = reinterpret_cast<const sockaddr *>(&bound); // NOLINT: POSIX socket API
    REQUIRE(::connect(fd, generic, static_cast<socklen_t>(sizeof(bound))) == 0);
    return fd;
}

} // namespace
#endif

TEST_CASE("epoll gateway availability is platform scoped", "[gateway][epoll]") {
#if defined(__linux__)
    REQUIRE(EpollServer::supported());
#else
    REQUIRE_FALSE(EpollServer::supported());
#endif
}

#if defined(__linux__)

TEST_CASE("epoll gateway handles multiple clients through one event loop", "[gateway][epoll]") {
    sockaddr_in bound{};
    const int listen_fd = bind_loopback_listener(bound);

    qsl::engine::MatchingEngine engine;
    engine.register_symbol("AAPL");
    OrderGateway gateway{engine, RiskConfig{1000, 1'000'000}};
    EpollServer server{gateway};
    std::atomic<bool> server_ok{false};

    std::thread server_thread([&] {
        server_ok.store(
            server.serve_listen_socket(
                listen_fd, EpollServerOptions{/*max_events=*/16, /*wait_timeout_ms=*/10}),
            std::memory_order_release);
    });

    const int client1 = connect_client(bound);
    const int client2 = connect_client(bound);

    write_all(client1,
              encode(NewOrder{1, 0, 100, 5, Side::Sell, OrderType::Limit, TimeInForce::GTC}, 1));
    const auto first = read_types(client1, 1);
    REQUIRE(first == std::vector<MsgType>{MsgType::Ack});

    write_all(client2, encode(Heartbeat{42}));
    const auto heartbeat = read_types(client2, 1);
    REQUIRE(heartbeat == std::vector<MsgType>{MsgType::HeartbeatAck});

    write_all(client2,
              encode(NewOrder{2, 0, 100, 5, Side::Buy, OrderType::Limit, TimeInForce::GTC}, 2));
    const auto cross = read_types(client2, 2);
    REQUIRE(cross == std::vector<MsgType>{MsgType::Ack, MsgType::Fill});

    write_all(client1, encode(Heartbeat{99}));
    const auto still_alive = read_types(client1, 1);
    REQUIRE(still_alive == std::vector<MsgType>{MsgType::HeartbeatAck});

    REQUIRE(::close(client1) == 0);
    REQUIRE(::close(client2) == 0);
    server.request_stop();
    server_thread.join();
    REQUIRE(server_ok.load(std::memory_order_acquire));
    REQUIRE(::close(listen_fd) == 0);
}

TEST_CASE("epoll gateway rejects invalid bind hosts", "[gateway][epoll]") {
    qsl::engine::MatchingEngine engine;
    engine.register_symbol("AAPL");
    OrderGateway gateway{engine, RiskConfig{1000, 1'000'000}};
    EpollServer server{gateway};

    REQUIRE_FALSE(server.run("localhost", 0));
    REQUIRE_FALSE(server.run("not-an-ip", 0));
}

#endif
