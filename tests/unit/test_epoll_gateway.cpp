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
#include <cstdint>
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

TEST_CASE("epoll gateway applies backpressure without dropping responses", "[gateway][epoll]") {
    sockaddr_in bound{};
    const int listen_fd = bind_loopback_listener(bound);

    qsl::engine::MatchingEngine engine;
    engine.register_symbol("AAPL");
    OrderGateway gateway{engine, RiskConfig{1000, 1'000'000}};
    EpollServer server{gateway};
    std::atomic<bool> server_ok{false};

    // A tiny outbound high-water mark forces the read-gating path: once a few responses are
    // buffered the server must stop reading (drop EPOLLIN) and resume only as the backlog drains.
    // The contract under that path is that no response is dropped or reordered and nothing
    // deadlocks.
    std::thread server_thread([&] {
        server_ok.store(server.serve_listen_socket(
                            listen_fd, EpollServerOptions{/*max_events=*/16, /*wait_timeout_ms=*/10,
                                                          /*max_outbuf_bytes=*/256}),
                        std::memory_order_release);
    });

    const int client = connect_client(bound);

    // Send many resting buy orders at one price (no cross -> exactly one Ack each) before reading
    // any response, so the server's outbound backlog repeatedly crosses the high-water mark.
    constexpr std::uint64_t kOrders = 64;
    for (std::uint64_t i = 1; i <= kOrders; ++i) {
        write_all(client,
                  encode(NewOrder{i, 0, 100, 5, Side::Buy, OrderType::Limit, TimeInForce::GTC}, i));
    }

    const auto types = read_types(client, static_cast<std::size_t>(kOrders));
    REQUIRE(types.size() == static_cast<std::size_t>(kOrders));
    REQUIRE(std::all_of(types.begin(), types.end(), [](MsgType t) { return t == MsgType::Ack; }));

    REQUIRE(::close(client) == 0);
    server.request_stop();
    server_thread.join();
    REQUIRE(server_ok.load(std::memory_order_acquire));
    REQUIRE(::close(listen_fd) == 0);
}

TEST_CASE("epoll gateway drops a non-reading client that exceeds the hard buffer cap",
          "[gateway][epoll]") {
    sockaddr_in bound{};
    const int listen_fd = bind_loopback_listener(bound);

    qsl::engine::MatchingEngine engine;
    engine.register_symbol("AAPL");
    OrderGateway gateway{engine, RiskConfig{1000, 1'000'000}};
    EpollServer server{gateway};
    std::atomic<bool> server_ok{false};

    // Tiny soft mark and hard cap: a single crossing order whose response (Ack + one Fill per
    // maker) exceeds the hard cap must make the server drop the connection rather than buffer it.
    std::thread server_thread([&] {
        server_ok.store(server.serve_listen_socket(
                            listen_fd, EpollServerOptions{/*max_events=*/16, /*wait_timeout_ms=*/10,
                                                          /*max_outbuf_bytes=*/64,
                                                          /*max_outbuf_hard_bytes=*/64}),
                        std::memory_order_release);
    });

    const int client = connect_client(bound);

    // Build a book of eight resting sells at one price, reading each Ack so the buffer stays low.
    constexpr std::uint64_t kMakers = 8;
    for (std::uint64_t i = 1; i <= kMakers; ++i) {
        write_all(
            client,
            encode(NewOrder{i, 0, 100, 5, Side::Sell, OrderType::Limit, TimeInForce::GTC}, i));
        REQUIRE(read_types(client, 1) == std::vector<MsgType>{MsgType::Ack});
    }

    // One crossing buy fills all eight makers -> Ack + 8 Fills, far past the 64-byte hard cap, so
    // the server enforces the cap before buffering and drops the connection instead.
    write_all(client, encode(NewOrder{kMakers + 1, 0, 100, 40, Side::Buy, OrderType::Limit,
                                      TimeInForce::GTC},
                             kMakers + 1));

    // The drop surfaces as a clean EOF (read returns 0) rather than the over-cap response. A
    // non-zero read here would mean the server buffered/sent it (cap not enforced); a -1 timeout
    // would mean it neither answered nor closed.
    std::array<std::byte, 512> buf{};
    ssize_t n = 0;
    for (int tries = 0; tries < 50; ++tries) {
        n = ::read(client, buf.data(), buf.size());
        if (n <= 0) {
            break;
        }
    }
    REQUIRE(n == 0); // server closed the connection (hard cap enforced)

    REQUIRE(::close(client) == 0);
    server.request_stop();
    server_thread.join();
    REQUIRE(server_ok.load(std::memory_order_acquire));
    REQUIRE(::close(listen_fd) == 0);
}

#endif
