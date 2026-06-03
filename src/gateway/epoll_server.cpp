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

bool send_some(int fd, std::vector<std::byte> &outbuf) {
    while (!outbuf.empty()) {
        const ssize_t n = ::send(fd, outbuf.data(), outbuf.size(), MSG_NOSIGNAL);
        if (n > 0) {
            outbuf.erase(outbuf.begin(), outbuf.begin() + static_cast<std::ptrdiff_t>(n));
            continue;
        }
        if (n < 0 && is_would_block()) {
            return true;
        }
        return false;
    }
    return true;
}

struct ClientState {
    explicit ClientState(OrderGateway &gateway) : session(gateway) {}

    Session session;
    std::vector<std::byte> outbuf;
    bool input_closed = false;
    bool close_after_flush = false;
};

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
                    if (errno == EINTR) {
                        continue;
                    }
                    return false;
                }
                continue;
            }

            auto it = clients.find(fd);
            if (it == clients.end()) {
                continue;
            }
            ClientState &client = it->second;
            bool close_now = false;

            if ((ev & EPOLLIN) != 0U) {
                std::array<std::byte, 4096> buffer{};
                for (;;) {
                    // Backpressure: once the outbound backlog reaches the high-water mark, stop
                    // reading *before* pulling in and processing another request, so additional
                    // requests cannot push the buffer further past the mark. Checked here (not only
                    // after appending) so the overshoot is bounded to the request already in
                    // flight. Note: a single response is still buffered whole -- e.g. a market
                    // order crossing a deep book yields one Fill per resting maker -- so the peak
                    // buffer is roughly this mark plus the largest single response, not a hard byte
                    // cap; the mark bounds how many *further* requests are read (see docs).
                    if (client.outbuf.size() >= options.max_outbuf_bytes) {
                        break;
                    }
                    const ssize_t n = ::read(fd, buffer.data(), buffer.size());
                    if (n > 0) {
                        auto response = client.session.on_bytes(
                            std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(n)));
                        client.outbuf.insert(client.outbuf.end(), response.begin(), response.end());
                        if (client.session.logged_out()) {
                            client.close_after_flush = true;
                            break;
                        }
                        continue; // re-checks the high-water mark at the top before reading again
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

            if (!close_now && (ev & EPOLLOUT) != 0U) {
                close_now = !send_some(fd, client.outbuf);
            }

            if (!close_now) {
                const bool want_write = !client.outbuf.empty();
                if (!want_write && (client.input_closed || client.close_after_flush ||
                                    (ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0U)) {
                    close_now = true; // fully flushed and the peer is done / has logged out
                } else {
                    std::uint32_t want = EPOLLRDHUP | EPOLLERR | EPOLLHUP;
                    // Backpressure: only keep reading while the write backlog is below the
                    // high-water mark, so a client that stops reading its responses cannot make the
                    // gateway buffer unbounded output. Reads resume once the backlog drains.
                    if (client.outbuf.size() < options.max_outbuf_bytes) {
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
