#include "qsl/gateway/epoll_server.hpp"

#include "qsl/gateway/session.hpp"

#if defined(__linux__)
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <fcntl.h>
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

bool set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool add_fd(int epoll_fd, int fd, std::uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    return ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == 0;
}

bool mod_fd(int epoll_fd, int fd, std::uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    return ::epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) == 0;
}

struct ClientState {
    explicit ClientState(OrderGateway &gateway) : session(gateway) {}

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

// Flush as much of the client's pending output as the nonblocking socket accepts. Advances a write
// offset rather than erasing from the front after every send -- erase-per-send is O(n^2) when
// draining a large buffer and can stall the single event loop. Returns false only on a real send
// error; EAGAIN/EWOULDBLOCK (socket full) and EINTR (signal) are normal and retryable.
bool send_some(int fd, ClientState &client) {
    while (client.sent < client.outbuf.size()) {
        const ssize_t n = ::send(fd, client.outbuf.data() + client.sent,
                                 client.outbuf.size() - client.sent, MSG_NOSIGNAL);
        if (n > 0) {
            client.sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && is_would_block()) {
            return true; // socket full: resume on the next EPOLLOUT
        }
        if (n < 0 && errno == EINTR) {
            continue; // interrupted by a signal: retry rather than dropping the connection
        }
        return false;
    }
    client.outbuf.clear(); // fully flushed
    client.sent = 0;
    return true;
}

} // namespace

bool EpollServer::serve_listen_socket(int listen_fd, EpollServerOptions options) {
    if (listen_fd < 0 || options.max_events == 0 || !set_nonblocking(listen_fd)) {
        return false;
    }

    UniqueFd epoll_fd = make_fd(::epoll_create1(EPOLL_CLOEXEC));
    if (*epoll_fd < 0) {
        return false;
    }
    if (!add_fd(*epoll_fd, listen_fd, EPOLLIN)) {
        return false;
    }

    std::vector<epoll_event> events(options.max_events);
    std::unordered_map<int, ClientState> clients;

    stop_requested_.store(false, std::memory_order_release);
    while (!stop_requested_.load(std::memory_order_acquire)) {
        const int ready = ::epoll_wait(*epoll_fd, events.data(), static_cast<int>(events.size()),
                                       options.wait_timeout_ms);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        for (int i = 0; i < ready; ++i) {
            const int fd = events[static_cast<std::size_t>(i)].data.fd;
            const std::uint32_t ev = events[static_cast<std::size_t>(i)].events;

            if (fd == listen_fd) {
                for (;;) {
                    const int conn = ::accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK);
                    if (conn >= 0) {
                        if (!add_fd(*epoll_fd, conn, EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP)) {
                            ::close(conn);
                            continue;
                        }
                        clients.try_emplace(conn, gateway_);
                        continue;
                    }
                    if (is_would_block()) {
                        break;
                    }
                    // Transient per-connection errors -- a connection aborted before accept, or a
                    // pending network error that Linux reports through accept -- must not tear down
                    // the whole server; skip this connection and keep serving the rest (accept(2)).
                    if (errno == EINTR || errno == ECONNABORTED || errno == EPROTO ||
                        errno == ENETDOWN || errno == ENOPROTOOPT || errno == EHOSTDOWN ||
                        errno == ENONET || errno == EHOSTUNREACH || errno == EOPNOTSUPP ||
                        errno == ENETUNREACH) {
                        continue;
                    }
                    return false; // genuinely fatal listener error
                }
                continue;
            }

            auto it = clients.find(fd);
            if (it == clients.end()) {
                continue;
            }
            ClientState &client = it->second;
            bool close_now = false;

            // Drain any writable backlog first, so the read below sees an up-to-date pending() and
            // the hard cap is not enforced against a stale buffer that send_some() could free
            // (which would otherwise falsely drop a client that has resumed reading).
            if ((ev & EPOLLOUT) != 0U) {
                close_now = !send_some(fd, client);
            }

            if (!close_now && (ev & EPOLLIN) != 0U) {
                std::array<std::byte, 4096> buffer{};
                for (;;) {
                    // Soft backpressure: once the unsent backlog reaches the soft high-water mark,
                    // stop reading more requests before processing another (the re-arm below drops
                    // EPOLLIN), resuming once it drains.
                    if (client.pending() >= options.max_outbuf_bytes) {
                        break;
                    }
                    const ssize_t n = ::read(fd, buffer.data(), buffer.size());
                    if (n > 0) {
                        auto response = client.session.on_bytes(
                            std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(n)));
                        client.drop_sent_prefix(); // reclaim already-sent bytes before growing
                        // Hard cap on the RETAINED buffer: if appending this response would push
                        // the connection past the hard limit, drop it instead of buffering, so the
                        // sustained per-connection buffer never exceeds the cap. The transient
                        // response vector is still materialized in full by Session::on_bytes for a
                        // single request; bounding that needs streaming response generation,
                        // tracked as a follow-up (see docs/socket_gateway.md).
                        if (options.max_outbuf_hard_bytes != 0 &&
                            client.outbuf.size() + response.size() >
                                options.max_outbuf_hard_bytes) {
                            close_now = true;
                            break;
                        }
                        client.outbuf.insert(client.outbuf.end(), response.begin(), response.end());
                        if (client.session.logged_out()) {
                            client.close_after_flush = true;
                            break;
                        }
                        continue; // re-checks the soft high-water mark at the top before reading
                    }
                    if (n == 0) {
                        client.input_closed = true;
                        break;
                    }
                    if (is_would_block()) {
                        break;
                    }
                    if (errno == EINTR) {
                        continue;
                    }
                    close_now = true;
                    break;
                }
            }

            if (!close_now) {
                const bool want_write = client.pending() > 0;
                if ((ev & (EPOLLERR | EPOLLHUP)) != 0U) {
                    // Hard error/hangup: the socket is dead, so drop it regardless of queued output
                    // (which can never be sent) instead of re-arming and waking the loop forever.
                    close_now = true;
                } else if (!want_write && (client.input_closed || client.close_after_flush ||
                                           (ev & EPOLLRDHUP) != 0U)) {
                    // Fully flushed and the peer is done / logged out / half-closed. RDHUP is a
                    // half-close: the peer can still receive queued responses, so keep flushing
                    // until want_write is false before closing.
                    close_now = true;
                } else {
                    std::uint32_t want = EPOLLRDHUP | EPOLLERR | EPOLLHUP;
                    // Backpressure: only keep reading while the unsent backlog is below the
                    // high-water mark, so a client that stops reading its responses cannot make the
                    // gateway buffer unbounded output. Reads resume once the backlog drains.
                    if (client.pending() < options.max_outbuf_bytes) {
                        want |= EPOLLIN;
                    }
                    if (want_write) {
                        want |= EPOLLOUT; // still have bytes to flush
                    }
                    close_now = !mod_fd(*epoll_fd, fd, want);
                }
            }

            if (close_now) {
                ::epoll_ctl(*epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                ::close(fd);
                clients.erase(it);
            }
        }
    }

    for (const auto &entry : clients) {
        const int fd = entry.first;
        ::epoll_ctl(*epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
        ::close(fd);
    }
    clients.clear();
    return true;
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
