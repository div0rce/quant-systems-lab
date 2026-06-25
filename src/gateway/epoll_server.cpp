#include "qsl/gateway/epoll_server.hpp"

#include "qsl/gateway/session.hpp"

#if defined(__linux__)
#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <netinet/in.h>
#include <span>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#endif

namespace qsl::gateway {

bool EpollServer::supported() noexcept {
#if defined(__linux__)
    return true;
#else
    return false;
#endif
}

#if defined(__linux__)
namespace {

inline constexpr std::uint32_t kListenerGeneration = 0;

struct FdCloser {
    void operator()(int *fd) const noexcept {
        if (fd != nullptr && *fd >= 0) {
            ::close(*fd);
        }
        delete fd;
    }
};

using UniqueFd = std::unique_ptr<int, FdCloser>;

UniqueFd make_fd(int fd) {
    return UniqueFd(new int(fd)); // NOLINT(cppcoreguidelines-owning-memory): small fd RAII shim
}

bool is_would_block() noexcept {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

bool event_has(std::uint32_t events, std::uint32_t flag) noexcept {
    return (events & flag) != 0U;
}

bool set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

std::uint64_t pack_event_data(int fd, std::uint32_t generation) noexcept {
    return (static_cast<std::uint64_t>(generation) << 32U) | static_cast<std::uint32_t>(fd);
}

int event_fd(const epoll_event &ev) noexcept {
    return static_cast<int>(static_cast<std::uint32_t>(ev.data.u64));
}

std::uint32_t event_generation(const epoll_event &ev) noexcept {
    return static_cast<std::uint32_t>(ev.data.u64 >> 32U);
}

std::uint32_t next_client_generation(std::uint32_t &next_generation) noexcept {
    const std::uint32_t generation = next_generation;
    ++next_generation;
    if (next_generation == kListenerGeneration) {
        ++next_generation;
    }
    return generation;
}

bool add_fd(int epoll_fd, int fd, std::uint32_t events, std::uint32_t generation) {
    epoll_event ev{};
    ev.events = events;
    ev.data.u64 = pack_event_data(fd, generation);
    return ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == 0;
}

bool mod_fd(int epoll_fd, int fd, std::uint32_t events, std::uint32_t generation) {
    epoll_event ev{};
    ev.events = events;
    ev.data.u64 = pack_event_data(fd, generation);
    return ::epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) == 0;
}

struct ClientState {
    ClientState(OrderGateway &gateway, std::uint32_t connection_generation)
        : generation(connection_generation), session(gateway) {}

    std::uint32_t generation;
    Session session;
    std::vector<std::byte> outbuf;
    std::size_t sent = 0; // bytes at the front of outbuf already written to the socket
    bool input_closed = false;
    bool close_after_flush = false;

    // Unsent bytes. The soft/hard limits and the "needs write" check use this, not outbuf.size().
    [[nodiscard]] std::size_t pending() const noexcept { return outbuf.size() - sent; }
    // Reclaim already-sent bytes from the front. Done once per append (amortized), never per send.
    void drop_sent_prefix() {
        if (sent > 0) {
            outbuf.erase(outbuf.begin(), outbuf.begin() + static_cast<std::ptrdiff_t>(sent));
            sent = 0;
        }
    }
};

using ClientMap = std::unordered_map<int, ClientState>;

bool has_pending_output(const ClientState &client) noexcept {
    return client.pending() > 0;
}

enum class SendResult { Progress, WouldBlock, Interrupted, Closed, Error };

SendResult send_next_chunk(int fd, ClientState &client) {
    const ssize_t n = ::send(fd, client.outbuf.data() + client.sent,
                             client.outbuf.size() - client.sent, MSG_NOSIGNAL);
    if (n > 0) {
        client.sent += static_cast<std::size_t>(n);
        return SendResult::Progress;
    }
    if (n == 0) {
        return SendResult::Closed;
    }
    if (is_would_block()) {
        return SendResult::WouldBlock;
    }
    if (errno == EINTR) {
        return SendResult::Interrupted;
    }
    return SendResult::Error;
}

// Flush as much of the client's pending output as the nonblocking socket accepts. Advances a write
// offset rather than erasing from the front after every send -- erase-per-send is O(n^2) when
// draining a large buffer and can stall the single event loop. Returns false only on a real send
// error; EAGAIN/EWOULDBLOCK (socket full) and EINTR (signal) are normal and retryable.
bool send_some(int fd, ClientState &client) {
    while (client.sent < client.outbuf.size()) {
        switch (send_next_chunk(fd, client)) {
        case SendResult::Progress:
        case SendResult::Interrupted:
            continue;
        case SendResult::WouldBlock:
            return true; // socket full: resume on the next EPOLLOUT
        case SendResult::Closed:
        case SendResult::Error:
            return false;
        }
    }
    client.outbuf.clear(); // fully flushed
    client.sent = 0;
    return true;
}

void close_client(int epoll_fd, int fd) noexcept {
    ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
}

void close_all_clients(int epoll_fd, ClientMap &clients) noexcept {
    for (const auto &entry : clients) {
        close_client(epoll_fd, entry.first);
    }
    clients.clear();
}

// Per-run event-loop state. Bundling these (instead of threading five-plus parameters through every
// handler) keeps each handler's signature small and the dispatch readable.
struct EventLoop {
    int epoll_fd;
    int listen_fd;
    EpollServerOptions options;
    OrderGateway &gateway;
    ClientMap clients{};
    std::uint32_t next_generation = 1;
    bool listener_armed = true; // false while paused on fd exhaustion (EMFILE/ENFILE)
};

// Arm/disarm the listener's EPOLLIN. On fd exhaustion we cannot accept (or even close) a new
// connection, and the listener is level-triggered, so leaving it armed would busy-spin epoll_wait.
// Disarming (MOD to 0 events) stops it being reported until a client fd frees and we re-arm — the
// server keeps serving existing clients instead of tearing down (Fatal) or spinning (Retry).
void set_listener_armed(EventLoop &loop, bool armed) {
    if (loop.listener_armed == armed) {
        return;
    }
    if (mod_fd(loop.epoll_fd, loop.listen_fd, armed ? EPOLLIN : 0U, kListenerGeneration)) {
        loop.listener_armed = armed;
    }
}

bool is_listener_event(const epoll_event &ev, int listen_fd) noexcept {
    return event_generation(ev) == kListenerGeneration && event_fd(ev) == listen_fd;
}

// A connection aborted before accept, or a pending network error Linux reports through accept(2),
// must not tear down the whole server -- skip the connection and keep serving the rest.
bool is_transient_accept_error() noexcept {
    static constexpr int kTransient[] = {EINTR,       ECONNABORTED, EPROTO, ENETDOWN,
                                         ENOPROTOOPT, EHOSTDOWN,    ENONET, EHOSTUNREACH,
                                         EOPNOTSUPP,  ENETUNREACH};
    return std::find(std::begin(kTransient), std::end(kTransient), errno) != std::end(kTransient);
}

bool prepare_listener_socket(int listen_fd, const EpollServerOptions &options) {
    if (listen_fd < 0) {
        return false;
    }
    if (options.max_events == 0) {
        return false;
    }
    return set_nonblocking(listen_fd);
}

// Build the epoll instance and register the (validated, nonblocking) listener. Returns an invalid
// fd holder on any failure; a partially-created epoll fd is closed on the way out by UniqueFd.
UniqueFd make_epoll_listener(int listen_fd, const EpollServerOptions &options) {
    if (!prepare_listener_socket(listen_fd, options)) {
        return make_fd(-1);
    }
    UniqueFd epoll_fd = make_fd(::epoll_create1(EPOLL_CLOEXEC));
    if (*epoll_fd < 0) {
        return make_fd(-1);
    }
    if (!add_fd(*epoll_fd, listen_fd, EPOLLIN, kListenerGeneration)) {
        return make_fd(-1);
    }
    return epoll_fd;
}

// Register an accepted connection: one Session per fd, armed for read/peer events. Returns false
// (leaving nothing in the map) if the slot or the epoll registration could not be created.
bool register_client(EventLoop &loop, int conn) {
    const std::uint32_t generation = next_client_generation(loop.next_generation);
    auto emplaced = loop.clients.try_emplace(conn, loop.gateway, generation);
    if (!emplaced.second ||
        !add_fd(loop.epoll_fd, conn, EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP, generation)) {
        loop.clients.erase(conn);
        return false;
    }
    return true;
}

enum class AcceptResult { Accepted, Drained, Retry, Fatal };

AcceptResult accept_one(EventLoop &loop) {
    const int conn = ::accept4(loop.listen_fd, nullptr, nullptr, SOCK_NONBLOCK);
    if (conn >= 0) {
        if (!register_client(loop, conn)) {
            ::close(conn);
        }
        return AcceptResult::Accepted;
    }
    if (is_would_block()) {
        return AcceptResult::Drained;
    }
    if (errno == EMFILE || errno == ENFILE) {
        // Out of file descriptors: pausing (not tearing down) keeps existing clients served. Disarm
        // the listener so the level-triggered fd stops waking epoll until a client frees an fd.
        set_listener_armed(loop, false);
        return AcceptResult::Drained;
    }
    if (is_transient_accept_error()) {
        return AcceptResult::Retry;
    }
    return AcceptResult::Fatal;
}

// max_accepts_per_tick with 0 normalized to "unbounded", so the accept loop tests a single bound.
std::size_t accepts_per_tick_limit(const EpollServerOptions &options) noexcept {
    return options.max_accepts_per_tick == 0 ? std::numeric_limits<std::size_t>::max()
                                             : options.max_accepts_per_tick;
}

// Accept pending connections, up to max_accepts_per_tick, then yield back to client I/O. Returns
// false only on a genuinely fatal listener error. The listener is level-triggered, so a backlog
// left after the per-tick bound is re-reported and accepted on the next event-loop iteration.
bool accept_connections(EventLoop &loop) {
    const std::size_t limit = accepts_per_tick_limit(loop.options);
    std::size_t accepted = 0;
    for (;;) {
        const AcceptResult result = accept_one(loop);
        if (result == AcceptResult::Fatal) {
            return false;
        }
        if (result == AcceptResult::Drained) {
            return true;
        }
        // fairness: once the per-tick bound is hit, stop hogging the loop turn and service ready
        // clients; the level-triggered listener re-reports any remaining backlog next iteration.
        if (result == AcceptResult::Accepted && ++accepted >= limit) {
            return true;
        }
        // Retry, or Accepted under the limit: keep accepting.
    }
}

enum class ReadResult { Continue, StopReading, CloseNow };

std::size_t hard_output_limit(const EpollServerOptions &options) noexcept {
    if (options.max_outbuf_hard_bytes == 0) {
        return std::numeric_limits<std::size_t>::max();
    }
    return options.max_outbuf_hard_bytes;
}

// Feed one read of client bytes through its Session, honoring the hard output cap. A frame whose
// response would exceed the cap is rejected: if earlier replies are already queued we flush them
// then close (StopReading + close_after_flush); otherwise we close immediately (CloseNow).
ReadResult apply_session_input(const EpollServerOptions &options, ClientState &client,
                               std::span<const std::byte> bytes) {
    client.drop_sent_prefix(); // reclaim already-sent bytes before growing outbuf
    const SessionStatus status =
        client.session.on_bytes(bytes, client.outbuf, hard_output_limit(options));
    if (status == SessionStatus::OutputLimitExceeded) {
        if (!client.outbuf.empty()) {
            client.close_after_flush = true;
            return ReadResult::StopReading;
        }
        return ReadResult::CloseNow;
    }
    if (client.session.logged_out()) {
        client.close_after_flush = true;
        return ReadResult::StopReading;
    }
    return ReadResult::Continue;
}

bool should_read_available_input(const EpollServerOptions &options, const ClientState &client,
                                 bool peer_hung_up) noexcept {
    return peer_hung_up || client.pending() < options.max_outbuf_bytes;
}

ReadResult read_once(const EpollServerOptions &options, int fd, ClientState &client,
                     std::span<std::byte> buffer) {
    const ssize_t n = ::read(fd, buffer.data(), buffer.size());
    if (n > 0) {
        return apply_session_input(
            options, client,
            std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(n)));
    }
    if (n == 0) {
        client.input_closed = true;
        return ReadResult::StopReading;
    }
    if (errno == EINTR) {
        return ReadResult::Continue;
    }
    if (is_would_block()) {
        return ReadResult::StopReading;
    }
    return ReadResult::CloseNow;
}

// Read everything currently available from the client. The loop guard applies soft backpressure:
// once the unsent backlog reaches the high-water mark (and the peer has not hung up) reading stops
// until the backlog drains. Returns true only if the connection must be closed (a read error).
bool read_available(const EpollServerOptions &options, int fd, ClientState &client,
                    bool peer_hung_up) {
    std::array<std::byte, 4096> buffer{};
    while (should_read_available_input(options, client, peer_hung_up)) {
        switch (read_once(options, fd, client, std::span<std::byte>(buffer))) {
        case ReadResult::Continue:
            continue;
        case ReadResult::StopReading:
            return false;
        case ReadResult::CloseNow:
            return true;
        }
    }
    return false; // soft backpressure: resume once the backlog drains
}

bool should_close_for_full_hangup(std::uint32_t events) noexcept {
    return event_has(events, EPOLLHUP);
}

void apply_peer_half_close(std::uint32_t events, ClientState &client) noexcept {
    if (event_has(events, EPOLLRDHUP)) {
        client.input_closed = true; // half-close: peer can still receive queued responses
    }
}

bool should_close_after_flush(const ClientState &client) noexcept {
    return !has_pending_output(client) && (client.input_closed || client.close_after_flush);
}

bool wants_read_interest(const EpollServerOptions &options, const ClientState &client) noexcept {
    return !client.input_closed && !client.close_after_flush &&
           client.pending() < options.max_outbuf_bytes;
}

std::uint32_t client_interests(const EpollServerOptions &options,
                               const ClientState &client) noexcept {
    std::uint32_t want = EPOLLRDHUP | EPOLLERR | EPOLLHUP;
    if (wants_read_interest(options, client)) {
        want |= EPOLLIN;
    }
    if (has_pending_output(client)) {
        want |= EPOLLOUT;
    }
    return want;
}

// Decide the client's next epoll interest after its event was serviced, or that it must close now.
bool rearm_client(EventLoop &loop, int fd, ClientState &client, std::uint32_t ev) {
    // EPOLLHUP can arrive with EPOLLIN for bytes already delivered; those were drained above. After
    // a hangup there is no receiving peer, so close instead of re-arming a hot HUP.
    if (should_close_for_full_hangup(ev)) {
        return true;
    }
    apply_peer_half_close(ev, client);
    if (should_close_after_flush(client)) {
        return true; // fully flushed and the peer is done / logged out
    }
    return !mod_fd(loop.epoll_fd, fd, client_interests(loop.options, client), client.generation);
}

ClientMap::iterator find_live_client(EventLoop &loop, int fd, std::uint32_t generation) {
    ClientMap::iterator it = loop.clients.find(fd);
    if (it == loop.clients.end() || it->second.generation != generation) {
        return loop.clients.end();
    }
    return it;
}

bool drain_writable_if_ready(std::uint32_t events, int fd, ClientState &client) {
    if (!event_has(events, EPOLLOUT)) {
        return false;
    }
    // Drain writable backlog first, so the read path sees an up-to-date pending() count.
    return !send_some(fd, client);
}

bool should_read_client_input(std::uint32_t events, const ClientState &client) noexcept {
    return event_has(events, EPOLLIN) && !client.input_closed && !client.close_after_flush;
}

bool read_client_input_if_ready(const EpollServerOptions &options, std::uint32_t events, int fd,
                                ClientState &client) {
    if (!should_read_client_input(events, client)) {
        return false;
    }
    return read_available(options, fd, client, event_has(events, EPOLLHUP));
}

bool service_client_event(EventLoop &loop, int fd, ClientState &client, std::uint32_t events) {
    if (event_has(events, EPOLLERR)) {
        return true;
    }
    if (drain_writable_if_ready(events, fd, client)) {
        return true;
    }
    if (read_client_input_if_ready(loop.options, events, fd, client)) {
        return true;
    }
    return rearm_client(loop, fd, client, events);
}

void close_client_entry(EventLoop &loop, ClientMap::iterator it) {
    close_client(loop.epoll_fd, it->first);
    loop.clients.erase(it);
    // A descriptor just freed: resume accepting if we had paused on fd exhaustion.
    set_listener_armed(loop, true);
}

// Service one ready event for an existing client: error -> drain writable backlog -> read ->
// re-arm/close. A stale event for a closed fd that was reused (generation mismatch) is ignored.
void process_client_event(EventLoop &loop, const epoll_event &ready_event) {
    const int fd = event_fd(ready_event);
    ClientMap::iterator it = find_live_client(loop, fd, event_generation(ready_event));
    if (it == loop.clients.end()) {
        return;
    }
    if (service_client_event(loop, fd, it->second, ready_event.events)) {
        close_client_entry(loop, it);
    }
}

bool dispatch_ready_event(EventLoop &loop, const epoll_event &ready_event) {
    if (is_listener_event(ready_event, loop.listen_fd)) {
        return accept_connections(loop);
    }
    process_client_event(loop, ready_event);
    return true;
}

bool dispatch_ready_events(EventLoop &loop, std::span<const epoll_event> ready_events) {
    for (const epoll_event &ready_event : ready_events) {
        if (!dispatch_ready_event(loop, ready_event)) {
            return false;
        }
    }
    return true;
}

bool run_event_loop(EventLoop &loop, std::vector<epoll_event> &events,
                    std::atomic<bool> &stop_requested) {
    while (!stop_requested.load(std::memory_order_acquire)) {
        const int ready =
            ::epoll_wait(loop.epoll_fd, events.data(), static_cast<int>(events.size()),
                         loop.options.wait_timeout_ms);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (!dispatch_ready_events(loop, std::span<const epoll_event>(
                                             events.data(), static_cast<std::size_t>(ready)))) {
            return false;
        }
    }
    return true;
}

} // namespace

bool EpollServer::serve_listen_socket(int listen_fd, EpollServerOptions options) {
    stop_requested_.store(false, std::memory_order_release);

    UniqueFd epoll_fd = make_epoll_listener(listen_fd, options);
    if (*epoll_fd < 0) {
        return false;
    }

    EventLoop loop{*epoll_fd, listen_fd, options, gateway_};
    std::vector<epoll_event> events(options.max_events);

    const bool ok = run_event_loop(loop, events, stop_requested_);
    close_all_clients(*epoll_fd, loop.clients);
    return ok;
}

bool EpollServer::run(const std::string &host, std::uint16_t port, EpollServerOptions options) {
    UniqueFd listen_fd = make_fd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0));
    if (*listen_fd < 0) {
        return false;
    }

    int yes = 1;
    ::setsockopt(*listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, static_cast<socklen_t>(sizeof(yes)));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    const int parsed = ::inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    if (parsed != 1) {
        return false;
    }

    auto *generic = reinterpret_cast<sockaddr *>(&addr); // NOLINT: POSIX socket API
    if (::bind(*listen_fd, generic, static_cast<socklen_t>(sizeof(addr))) < 0 ||
        ::listen(*listen_fd, 128) < 0) {
        return false;
    }
    return serve_listen_socket(*listen_fd, options);
}

#else

bool EpollServer::serve_listen_socket(int, EpollServerOptions) {
    static_cast<void>(gateway_);
    return false;
}

bool EpollServer::run(const std::string &, std::uint16_t, EpollServerOptions) {
    static_cast<void>(gateway_);
    return false;
}

#endif

} // namespace qsl::gateway
