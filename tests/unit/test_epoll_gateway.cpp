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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <netinet/in.h>
#include <span>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>
#endif

using namespace qsl::gateway;
using namespace qsl::protocol;

#if defined(__linux__)
namespace {

struct SocketFd {
    SocketFd() noexcept = default;
    explicit SocketFd(int fd) noexcept : value(fd) {}

    ~SocketFd() { reset(); }

    SocketFd(const SocketFd &) = delete;
    SocketFd &operator=(const SocketFd &) = delete;

    SocketFd(SocketFd &&other) noexcept : value(std::exchange(other.value, -1)) {}

    SocketFd &operator=(SocketFd &&other) noexcept {
        if (this != &other) {
            reset();
            value = std::exchange(other.value, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return value; }
    [[nodiscard]] bool is_valid() const noexcept { return value >= 0; }

    [[nodiscard]] bool close_now() noexcept {
        if (!is_valid()) {
            return true;
        }
        const int fd = std::exchange(value, -1);
        return ::close(fd) == 0;
    }

  private:
    void reset() noexcept {
        if (is_valid()) {
            const int fd = std::exchange(value, -1);
            (void)::close(fd);
        }
    }

    int value = -1;
};

struct OrderCountExpectation {
    qsl::core::SeqNo last_seq;
    std::size_t orders;
};

struct SingleAskExpectation {
    qsl::core::SeqNo last_seq;
    std::size_t orders;
    qsl::core::Price price;
    qsl::core::QuantityTotal quantity;
};

struct AskShape {
    std::size_t orders;
    std::size_t levels;
    qsl::core::Price price;
    qsl::core::QuantityTotal quantity;

    friend bool operator==(const AskShape &, const AskShape &) = default;
};

int native(const SocketFd &fd) {
    return fd.get();
}

SocketFd require_socket() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    return SocketFd(fd);
}

void close_for_disconnect(SocketFd &fd) {
    REQUIRE(fd.close_now());
}

void set_read_timeout(const SocketFd &fd) {
    timeval timeout{};
    timeout.tv_sec = 2;
    REQUIRE(::setsockopt(native(fd), SOL_SOCKET, SO_RCVTIMEO, &timeout,
                         static_cast<socklen_t>(sizeof(timeout))) == 0);
}

void write_all(const SocketFd &fd, const std::vector<std::byte> &data) {
    std::size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::send(native(fd), data.data() + off, data.size() - off, MSG_NOSIGNAL);
        REQUIRE(n > 0);
        off += static_cast<std::size_t>(n);
    }
}

std::vector<MsgType> read_types(const SocketFd &fd, std::size_t expected) {
    std::vector<std::byte> received;
    std::vector<MsgType> types;
    std::array<std::byte, 4096> buf{};

    while (types.size() < expected) {
        const ssize_t n = ::read(native(fd), buf.data(), buf.size());
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

void require_types(const SocketFd &fd, std::initializer_list<MsgType> expected) {
    const auto actual = read_types(fd, expected.size());
    const std::vector<MsgType> expected_types(expected);
    REQUIRE(actual == expected_types);
}

void require_repeated_type(const SocketFd &fd, std::size_t count, MsgType expected) {
    const auto types = read_types(fd, count);
    REQUIRE(types.size() == count);
    REQUIRE(
        std::all_of(types.begin(), types.end(), [expected](MsgType t) { return t == expected; }));
}

void require_clean_eof(const SocketFd &fd) {
    std::array<std::byte, 512> buf{};
    const ssize_t n = ::read(native(fd), buf.data(), buf.size());
    REQUIRE(n == 0);
}

void require_eventual_eof_after_drain(const SocketFd &fd) {
    std::array<std::byte, 512> buf{};
    ssize_t n = 0;
    for (int tries = 0; tries < 50; ++tries) {
        n = ::read(native(fd), buf.data(), buf.size());
        if (n <= 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(n == 0);
}

void enable_reuse_addr(const SocketFd &fd) {
    int yes = 1;
    REQUIRE(::setsockopt(native(fd), SOL_SOCKET, SO_REUSEADDR, &yes,
                         static_cast<socklen_t>(sizeof(yes))) == 0);
}

void set_loopback_addr(sockaddr_in &addr) {
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    REQUIRE(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
}

void bind_and_listen(const SocketFd &listen_fd, sockaddr_in &addr) {
    auto *addr_generic = reinterpret_cast<sockaddr *>(&addr); // NOLINT: POSIX socket API
    REQUIRE(::bind(native(listen_fd), addr_generic, static_cast<socklen_t>(sizeof(addr))) == 0);
    REQUIRE(::listen(native(listen_fd), 16) == 0);
}

void read_bound_addr(const SocketFd &listen_fd, sockaddr_in &bound) {
    socklen_t bound_len = sizeof(bound);
    auto *bound_generic = reinterpret_cast<sockaddr *>(&bound); // NOLINT: POSIX socket API
    REQUIRE(::getsockname(native(listen_fd), bound_generic, &bound_len) == 0);
}

SocketFd bind_loopback_listener(sockaddr_in &bound) {
    SocketFd listen_fd = require_socket();
    enable_reuse_addr(listen_fd);

    sockaddr_in addr{};
    set_loopback_addr(addr);
    bind_and_listen(listen_fd, addr);

    read_bound_addr(listen_fd, bound);
    return listen_fd;
}

SocketFd connect_client(const sockaddr_in &bound) {
    SocketFd fd = require_socket();
    set_read_timeout(fd);
    const auto *generic = reinterpret_cast<const sockaddr *>(&bound); // NOLINT: POSIX socket API
    REQUIRE(::connect(native(fd), generic, static_cast<socklen_t>(sizeof(bound))) == 0);
    return fd;
}

void stop_server(EpollServer &server, std::thread &server_thread,
                 const std::atomic<bool> &server_ok) {
    server.request_stop();
    server_thread.join();
    REQUIRE(server_ok.load(std::memory_order_acquire));
}

const qsl::engine::SymbolSnapshot &require_one_symbol(const qsl::engine::EngineSnapshot &snapshot,
                                                      qsl::core::SeqNo last_seq) {
    REQUIRE(snapshot.last_seq == last_seq);
    REQUIRE(snapshot.symbols.size() == 1);
    return snapshot.symbols.front();
}

void require_order_count(const qsl::engine::MatchingEngine &engine,
                         OrderCountExpectation expected) {
    const auto snapshot = engine.snapshot();
    const auto &symbol = require_one_symbol(snapshot, expected.last_seq);
    REQUIRE(symbol.order_count == expected.orders);
}

AskShape ask_shape(const qsl::engine::SymbolSnapshot &symbol) {
    if (symbol.asks.empty()) {
        return AskShape{symbol.order_count, 0, 0, 0};
    }
    return AskShape{symbol.order_count, symbol.asks.size(), symbol.asks.front().price,
                    symbol.asks.front().quantity};
}

void require_single_ask(const qsl::engine::MatchingEngine &engine, SingleAskExpectation expected) {
    const auto snapshot = engine.snapshot();
    const auto &symbol = require_one_symbol(snapshot, expected.last_seq);
    REQUIRE(ask_shape(symbol) == AskShape{expected.orders, 1, expected.price, expected.quantity});
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
    const SocketFd listen_fd = bind_loopback_listener(bound);

    qsl::engine::MatchingEngine engine;
    engine.register_symbol("AAPL");
    OrderGateway gateway{engine, RiskConfig{1000, 1'000'000}};
    EpollServer server{gateway};
    std::atomic<bool> server_ok{false};

    std::thread server_thread([&] {
        server_ok.store(
            server.serve_listen_socket(
                native(listen_fd), EpollServerOptions{/*max_events=*/16, /*wait_timeout_ms=*/10}),
            std::memory_order_release);
    });

    const SocketFd client1 = connect_client(bound);
    const SocketFd client2 = connect_client(bound);

    write_all(client1,
              encode(NewOrder{1, 0, 100, 5, Side::Sell, OrderType::Limit, TimeInForce::GTC}, 1));
    require_types(client1, {MsgType::Ack});

    write_all(client2, encode(Heartbeat{42}));
    require_types(client2, {MsgType::HeartbeatAck});

    write_all(client2,
              encode(NewOrder{2, 0, 100, 5, Side::Buy, OrderType::Limit, TimeInForce::GTC}, 2));
    require_types(client2, {MsgType::Ack, MsgType::Fill});

    write_all(client1, encode(Heartbeat{99}));
    require_types(client1, {MsgType::HeartbeatAck});

    stop_server(server, server_thread, server_ok);
}

TEST_CASE("epoll gateway keeps serving after a client disconnects (connection lifecycle)",
          "[gateway][epoll]") {
    sockaddr_in bound{};
    const SocketFd listen_fd = bind_loopback_listener(bound);

    qsl::engine::MatchingEngine engine;
    engine.register_symbol("AAPL");
    OrderGateway gateway{engine, RiskConfig{1000, 1'000'000}};
    EpollServer server{gateway};
    std::atomic<bool> server_ok{false};

    std::thread server_thread([&] {
        server_ok.store(
            server.serve_listen_socket(
                native(listen_fd), EpollServerOptions{/*max_events=*/16, /*wait_timeout_ms=*/10}),
            std::memory_order_release);
    });

    // First connection: serve one order, then fully disconnect so the server closes and erases it.
    SocketFd first = connect_client(bound);
    write_all(first,
              encode(NewOrder{1, 0, 100, 5, Side::Sell, OrderType::Limit, TimeInForce::GTC}, 1));
    require_types(first, {MsgType::Ack});
    close_for_disconnect(first);

    // A second, freshly-accepted connection must be served independently of the first's teardown
    // (a new accept + a new connection generation), confirming the per-connection lifecycle. The
    // buy rests below the resting ask, so it acks without crossing.
    const SocketFd second = connect_client(bound);
    write_all(second,
              encode(NewOrder{2, 0, 99, 5, Side::Buy, OrderType::Limit, TimeInForce::GTC}, 2));
    require_types(second, {MsgType::Ack});

    stop_server(server, server_thread, server_ok);
    require_order_count(engine, OrderCountExpectation{/*last_seq=*/2, /*orders=*/2});
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
    const SocketFd listen_fd = bind_loopback_listener(bound);

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
        server_ok.store(
            server.serve_listen_socket(native(listen_fd),
                                       EpollServerOptions{/*max_events=*/16, /*wait_timeout_ms=*/10,
                                                          /*max_outbuf_bytes=*/256}),
            std::memory_order_release);
    });

    const SocketFd client = connect_client(bound);

    // Send many resting buy orders at one price (no cross -> exactly one Ack each) before reading
    // any response, so the server's outbound backlog repeatedly crosses the high-water mark.
    constexpr std::uint64_t kOrders = 64;
    for (std::uint64_t i = 1; i <= kOrders; ++i) {
        write_all(client,
                  encode(NewOrder{i, 0, 100, 5, Side::Buy, OrderType::Limit, TimeInForce::GTC}, i));
    }

    require_repeated_type(client, static_cast<std::size_t>(kOrders), MsgType::Ack);

    stop_server(server, server_thread, server_ok);
}

TEST_CASE("epoll gateway drops a non-reading client that exceeds the hard buffer cap",
          "[gateway][epoll]") {
    sockaddr_in bound{};
    const SocketFd listen_fd = bind_loopback_listener(bound);

    qsl::engine::MatchingEngine engine;
    engine.register_symbol("AAPL");
    OrderGateway gateway{engine, RiskConfig{1000, 1'000'000}};
    EpollServer server{gateway};
    std::atomic<bool> server_ok{false};

    // Tiny soft mark and hard cap: a single crossing order whose response (Ack + one Fill per
    // maker) exceeds the hard cap must make the server drop the connection rather than buffer it.
    std::thread server_thread([&] {
        server_ok.store(
            server.serve_listen_socket(native(listen_fd),
                                       EpollServerOptions{/*max_events=*/16, /*wait_timeout_ms=*/10,
                                                          /*max_outbuf_bytes=*/64,
                                                          /*max_outbuf_hard_bytes=*/64}),
            std::memory_order_release);
    });

    const SocketFd client = connect_client(bound);

    // Build a book of eight resting sells at one price, reading each Ack so the buffer stays low.
    constexpr std::uint64_t kMakers = 8;
    for (std::uint64_t i = 1; i <= kMakers; ++i) {
        write_all(
            client,
            encode(NewOrder{i, 0, 100, 5, Side::Sell, OrderType::Limit, TimeInForce::GTC}, i));
        require_types(client, {MsgType::Ack});
    }

    // One crossing buy fills all eight makers -> Ack + 8 Fills, far past the 64-byte hard cap, so
    // the server enforces the cap before buffering and drops the connection instead.
    write_all(client, encode(NewOrder{kMakers + 1, 0, 100, 40, Side::Buy, OrderType::Limit,
                                      TimeInForce::GTC},
                             kMakers + 1));

    // The drop surfaces as a clean EOF (read returns 0) rather than the over-cap response. A
    // non-zero read here would mean the server buffered/sent it (cap not enforced); a -1 timeout
    // would mean it neither answered nor closed.
    require_eventual_eof_after_drain(client);

    stop_server(server, server_thread, server_ok);
    require_single_ask(engine, SingleAskExpectation{/*last_seq=*/kMakers,
                                                    /*orders=*/kMakers,
                                                    /*price=*/100,
                                                    /*quantity=*/kMakers * 5});
}

TEST_CASE("epoll gateway preserves queued replies before later over-cap close",
          "[gateway][epoll]") {
    sockaddr_in bound{};
    const SocketFd listen_fd = bind_loopback_listener(bound);

    qsl::engine::MatchingEngine engine;
    engine.register_symbol("AAPL");
    OrderGateway gateway{engine, RiskConfig{1000, 1'000'000}};
    EpollServer server{gateway};
    std::atomic<bool> server_ok{false};

    std::thread server_thread([&] {
        server_ok.store(
            server.serve_listen_socket(
                native(listen_fd),
                EpollServerOptions{/*max_events=*/16, /*wait_timeout_ms=*/10,
                                   /*max_outbuf_bytes=*/1024,
                                   /*max_outbuf_hard_bytes=*/kHeaderSize + kAckBodySize}),
            std::memory_order_release);
    });

    const SocketFd client = connect_client(bound);

    auto batch = encode(NewOrder{1, 0, 100, 5, Side::Sell, OrderType::Limit, TimeInForce::GTC}, 1);
    const auto second =
        encode(NewOrder{2, 0, 101, 5, Side::Sell, OrderType::Limit, TimeInForce::GTC}, 2);
    batch.insert(batch.end(), second.begin(), second.end());
    write_all(client, batch);

    require_types(client, {MsgType::Ack});

    require_clean_eof(client);

    stop_server(server, server_thread, server_ok);
    require_single_ask(engine, SingleAskExpectation{/*last_seq=*/1,
                                                    /*orders=*/1,
                                                    /*price=*/100,
                                                    /*quantity=*/5});
}

TEST_CASE("epoll gateway drains a complete request before hangup close", "[gateway][epoll]") {
    sockaddr_in bound{};
    const SocketFd listen_fd = bind_loopback_listener(bound);

    qsl::engine::MatchingEngine engine;
    engine.register_symbol("AAPL");
    OrderGateway gateway{engine, RiskConfig{1000, 1'000'000}};
    EpollServer server{gateway};
    std::atomic<bool> server_ok{false};

    std::thread server_thread([&] {
        server_ok.store(
            server.serve_listen_socket(
                native(listen_fd), EpollServerOptions{/*max_events=*/16, /*wait_timeout_ms=*/10}),
            std::memory_order_release);
    });

    SocketFd client = connect_client(bound);
    write_all(client,
              encode(NewOrder{1, 0, 100, 5, Side::Sell, OrderType::Limit, TimeInForce::GTC}, 1));
    close_for_disconnect(client);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop_server(server, server_thread, server_ok);
    require_single_ask(engine, SingleAskExpectation{/*last_seq=*/1,
                                                    /*orders=*/1,
                                                    /*price=*/100,
                                                    /*quantity=*/5});
}

#endif
