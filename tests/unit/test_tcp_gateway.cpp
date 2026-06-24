#include "qsl/engine/matching_engine.hpp"
#include "qsl/gateway/order_gateway.hpp"
#include "qsl/gateway/tcp_server.hpp"
#include "qsl/protocol/codec.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <netinet/in.h>
#include <span>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace qsl::gateway;
using namespace qsl::protocol;

namespace {
void write_all(int fd, const std::vector<std::byte> &data) {
    std::size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        REQUIRE(n > 0);
        off += static_cast<std::size_t>(n);
    }
}

std::vector<std::byte> read_all(int fd) {
    std::vector<std::byte> received;
    std::array<std::byte, 4096> buf{};
    while (true) {
        const ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n <= 0) {
            break;
        }
        received.insert(received.end(), buf.begin(), buf.begin() + n);
    }
    return received;
}

bool is_heartbeat_ack(std::span<const std::byte> bytes, std::uint64_t token) {
    if (bytes.size() != kHeaderSize + kHeartbeatBodySize) {
        return false;
    }
    const auto ack = decode_heartbeat_ack(bytes);
    return ack.ok() && ack.value.token == token;
}

void set_recv_timeout(int fd, std::chrono::milliseconds timeout) {
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    REQUIRE(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, static_cast<socklen_t>(sizeof(tv))) ==
            0);
}
} // namespace

// Single-threaded localhost loopback: the TCP handshake completes in the kernel backlog, so
// connect() returns before accept(), and the client sends everything (then SHUT_WR) before
// the server serves the connection. No threads, no timeouts.
TEST_CASE("tcp loopback: orders and heartbeat over a real socket", "[gateway][tcp]") {
    const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listen_fd >= 0);
    int yes = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, static_cast<socklen_t>(sizeof(yes)));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0; // ephemeral
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    auto *addr_generic = reinterpret_cast<sockaddr *>(&addr); // NOLINT: POSIX socket API
    REQUIRE(::bind(listen_fd, addr_generic, static_cast<socklen_t>(sizeof(addr))) == 0);
    REQUIRE(::listen(listen_fd, 1) == 0);

    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    auto *bound_generic = reinterpret_cast<sockaddr *>(&bound); // NOLINT: POSIX socket API
    REQUIRE(::getsockname(listen_fd, bound_generic, &bound_len) == 0);

    const int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(client_fd >= 0);
    REQUIRE(::connect(client_fd, bound_generic, static_cast<socklen_t>(sizeof(bound))) == 0);

    const int conn = ::accept(listen_fd, nullptr, nullptr);
    REQUIRE(conn >= 0);

    // Client: rest a sell, send a crossing buy, and a heartbeat; then signal end-of-requests.
    write_all(client_fd,
              encode(NewOrder{1, 0, 100, 5, Side::Sell, OrderType::Limit, TimeInForce::GTC}, 1));
    write_all(client_fd,
              encode(NewOrder{2, 0, 100, 5, Side::Buy, OrderType::Limit, TimeInForce::GTC}, 2));
    write_all(client_fd, encode(Heartbeat{99}));
    ::shutdown(client_fd, SHUT_WR);

    qsl::engine::MatchingEngine engine;
    engine.register_symbol("AAPL");
    OrderGateway gateway{engine, RiskConfig{1000, 1'000'000}};
    TcpServer server{gateway};
    server.serve_connection(conn); // reads all, replies, returns on client EOF
    ::shutdown(conn, SHUT_WR);

    const std::vector<std::byte> received = read_all(client_fd);
    ::close(conn);
    ::close(client_fd);
    ::close(listen_fd);

    std::vector<MsgType> types;
    std::size_t off = 0;
    while (off + kHeaderSize <= received.size()) {
        const auto header = decode_header(std::span<const std::byte>(received).subspan(off));
        REQUIRE(header.error == DecodeError::None);
        const std::size_t total = kHeaderSize + header.value.body_len;
        types.push_back(header.value.type);
        off += total;
    }
    const auto has = [&](MsgType t) {
        return std::find(types.begin(), types.end(), t) != types.end();
    };
    REQUIRE(has(MsgType::Ack));
    REQUIRE(has(MsgType::Fill));
    REQUIRE(has(MsgType::HeartbeatAck));
}

TEST_CASE("tcp server bounds high-fanout responses before gateway mutation", "[gateway][tcp]") {
    int fds[2]{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    qsl::engine::MatchingEngine engine;
    engine.register_symbol("AAPL");
    for (std::uint64_t i = 1; i <= 8; ++i) {
        static_cast<void>(engine.new_limit(0, i, Side::Sell, 100, 1, TimeInForce::GTC));
    }
    const auto before = engine.snapshot();

    std::vector<std::byte> request = encode(Heartbeat{42});
    const NewOrder sweep{9, 0, 0, 8, Side::Buy, OrderType::Market, TimeInForce::GTC};
    const auto sweep_frame = encode(sweep, 9);
    request.insert(request.end(), sweep_frame.begin(), sweep_frame.end());
    write_all(fds[1], request);
    static_cast<void>(::shutdown(fds[1], SHUT_WR));

    TcpServerOptions options;
    options.max_response_bytes = kHeaderSize + kHeartbeatBodySize;
    OrderGateway gateway{engine, RiskConfig{1000, 1'000'000}};
    TcpServer server{gateway, options};
    server.serve_connection(fds[0]);
    static_cast<void>(::shutdown(fds[0], SHUT_WR));

    const std::vector<std::byte> received = read_all(fds[1]);
    REQUIRE(is_heartbeat_ack(received, 42));
    REQUIRE(engine.snapshot() == before);

    static_cast<void>(::close(fds[0]));
    static_cast<void>(::close(fds[1]));
}

TEST_CASE("tcp server accepts a second client while the first remains open", "[gateway][tcp]") {
    const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listen_fd >= 0);
    int yes = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, static_cast<socklen_t>(sizeof(yes)));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    auto *addr_generic = reinterpret_cast<sockaddr *>(&addr); // NOLINT: POSIX socket API
    REQUIRE(::bind(listen_fd, addr_generic, static_cast<socklen_t>(sizeof(addr))) == 0);
    REQUIRE(::listen(listen_fd, 2) == 0);

    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    auto *bound_generic = reinterpret_cast<sockaddr *>(&bound); // NOLINT: POSIX socket API
    REQUIRE(::getsockname(listen_fd, bound_generic, &bound_len) == 0);

    qsl::engine::MatchingEngine engine;
    engine.register_symbol("AAPL");
    OrderGateway gateway{engine, RiskConfig{1000, 1'000'000}};
    TcpServerOptions options;
    options.max_accepts = 2;
    TcpServer server{gateway, options};

    bool server_ok = false;
    std::thread server_thread([&] { server_ok = server.serve_listen_socket(listen_fd); });

    const int first = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(first >= 0);
    REQUIRE(::connect(first, bound_generic, static_cast<socklen_t>(sizeof(bound))) == 0);
    write_all(first, encode(Heartbeat{1}));

    const int second = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(second >= 0);
    set_recv_timeout(second, std::chrono::milliseconds(1000));
    REQUIRE(::connect(second, bound_generic, static_cast<socklen_t>(sizeof(bound))) == 0);
    write_all(second, encode(Heartbeat{2}));
    static_cast<void>(::shutdown(second, SHUT_WR));

    const std::vector<std::byte> second_response = read_all(second);
    REQUIRE(is_heartbeat_ack(second_response, 2));

    static_cast<void>(::shutdown(first, SHUT_WR));
    static_cast<void>(::close(first));
    static_cast<void>(::close(second));
    server_thread.join();
    static_cast<void>(::close(listen_fd));

    REQUIRE(server_ok);
}

TEST_CASE("tcp server caps concurrent connections and sheds load past the cap", "[gateway][tcp]") {
    const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listen_fd >= 0);
    int yes = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, static_cast<socklen_t>(sizeof(yes)));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    auto *addr_generic = reinterpret_cast<sockaddr *>(&addr); // NOLINT: POSIX socket API
    REQUIRE(::bind(listen_fd, addr_generic, static_cast<socklen_t>(sizeof(addr))) == 0);
    REQUIRE(::listen(listen_fd, 4) == 0);

    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    auto *bound_generic = reinterpret_cast<sockaddr *>(&bound); // NOLINT: POSIX socket API
    REQUIRE(::getsockname(listen_fd, bound_generic, &bound_len) == 0);

    qsl::engine::MatchingEngine engine;
    engine.register_symbol("AAPL");
    OrderGateway gateway{engine, RiskConfig{1000, 1'000'000}};
    TcpServerOptions options;
    options.max_accepts = 2;            // return after two *served* connections
    options.max_active_connections = 1; // one worker at a time; over-cap connections are shed
    TcpServer server{gateway, options};

    bool server_ok = false;
    std::thread server_thread([&] { server_ok = server.serve_listen_socket(listen_fd); });

    const auto connect_fd = [&] {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(fd >= 0);
        // Generous so a loaded CI host (e.g. right after the build) cannot trip a spurious EAGAIN
        // on the worker's reply; the happy path replies in microseconds and never waits this long.
        set_recv_timeout(fd, std::chrono::milliseconds(5000));
        REQUIRE(::connect(fd, bound_generic, static_cast<socklen_t>(sizeof(bound))) == 0);
        return fd;
    };

    // First client takes the single slot and is held open, so its worker blocks in read() after
    // replying. Reading its HeartbeatAck is the sync point: the slot is now full.
    const int first = connect_fd();
    write_all(first, encode(Heartbeat{1}));
    std::array<std::byte, kHeaderSize + kHeartbeatBodySize> ack{};
    std::size_t got = 0;
    while (got < ack.size()) {
        const ssize_t n = ::read(first, ack.data() + got, ack.size() - got);
        REQUIRE(n > 0);
        got += static_cast<std::size_t>(n);
    }
    REQUIRE(is_heartbeat_ack(std::span<const std::byte>(ack), 1));

    // A second client connects while the slot is full: the server sheds it (closes without
    // serving), so its read returns EOF immediately with no response. No write -> no SIGPIPE race.
    const int second = connect_fd();
    REQUIRE(read_all(second).empty());
    static_cast<void>(::close(second));

    // Free the slot; a later client is then served, proving the cap re-admits once a worker drains.
    // The retry absorbs the brief reap window between the first worker finishing and the next
    // accept (a shed connection returns EOF fast, so this loop is cheap).
    static_cast<void>(::shutdown(first, SHUT_WR));
    bool served_after_free = false;
    for (int attempt = 0; attempt < 100 && !served_after_free; ++attempt) {
        const int retry = connect_fd();
        write_all(retry, encode(Heartbeat{3}));
        static_cast<void>(::shutdown(retry, SHUT_WR));
        served_after_free = is_heartbeat_ack(read_all(retry), 3);
        static_cast<void>(::close(retry));
    }
    REQUIRE(served_after_free);

    static_cast<void>(::close(first));
    server_thread.join();
    static_cast<void>(::close(listen_fd));
    REQUIRE(server_ok);
}

TEST_CASE("tcp server rejects non-numeric bind hosts", "[gateway][tcp]") {
    qsl::engine::MatchingEngine engine;
    engine.register_symbol("AAPL");
    OrderGateway gateway{engine, RiskConfig{1000, 1'000'000}};
    TcpServer server{gateway};

    REQUIRE_FALSE(server.run("localhost", 0));
    REQUIRE_FALSE(server.run("not-an-ip", 0));
}

TEST_CASE("tcp server survives a peer closing before response write", "[gateway][tcp]") {
    int fds[2]{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    write_all(fds[1],
              encode(NewOrder{1, 0, 100, 5, Side::Buy, OrderType::Limit, TimeInForce::GTC}, 1));
    REQUIRE(::close(fds[1]) == 0);

    qsl::engine::MatchingEngine engine;
    engine.register_symbol("AAPL");
    OrderGateway gateway{engine, RiskConfig{1000, 1'000'000}};
    TcpServer server{gateway};
    server.serve_connection(fds[0]);

    REQUIRE(::close(fds[0]) == 0);
}
