#include "qsl/gateway/tcp_server.hpp"

#include "qsl/gateway/session.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <limits>
#include <memory>
#include <netinet/in.h>
#include <span>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace qsl::gateway {
namespace {

std::size_t response_limit(TcpServerOptions options) noexcept {
    return options.max_response_bytes == 0 ? std::numeric_limits<std::size_t>::max()
                                           : options.max_response_bytes;
}

// Write all bytes, tolerating partial writes. std::byte* converts implicitly to void*.
bool write_all(int fd, std::span<const std::byte> data) {
#if defined(SO_NOSIGPIPE)
    int yes = 1;
    static_cast<void>(
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, static_cast<socklen_t>(sizeof(yes))));
#endif
    std::size_t offset = 0;
    while (offset < data.size()) {
#if defined(MSG_NOSIGNAL)
        const ssize_t n = ::send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
#else
        // Some platforms, including macOS, suppress SIGPIPE through SO_NOSIGPIPE above.
        // Where neither mechanism exists, send() still reports write failure normally after
        // the platform's default signal handling.
        const ssize_t n = ::send(fd, data.data() + offset, data.size() - offset, 0);
#endif
        if (n < 0) {
            if (errno == EINTR) {
                continue; // a signal interrupted send() before any bytes moved: retry, don't drop
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        offset += static_cast<std::size_t>(n);
    }
    return true;
}

struct ConnectionWorker {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
};

void join_finished(std::vector<ConnectionWorker> &workers) {
    for (auto it = workers.begin(); it != workers.end();) {
        if (it->done->load(std::memory_order_acquire)) {
            it->thread.join();
            it = workers.erase(it);
        } else {
            ++it;
        }
    }
}

void join_all(std::vector<ConnectionWorker> &workers) {
    for (auto &worker : workers) {
        worker.thread.join();
    }
    workers.clear();
}

// read() that retries on EINTR (a signal interruption is not a disconnect). Returns bytes read
// (>0), 0 on EOF, or -1 on a real error.
ssize_t read_retry(int fd, std::span<std::byte> buffer) {
    for (;;) {
        const ssize_t n = ::read(fd, buffer.data(), buffer.size());
        if (n >= 0 || errno != EINTR) {
            return n;
        }
    }
}

// Admission control: true when the worker count has reached a nonzero cap.
bool at_capacity(const std::vector<ConnectionWorker> &workers, std::size_t cap) noexcept {
    return cap != 0 && workers.size() >= cap;
}

// accept() can fail transiently without the listener being broken: a signal (EINTR) or a peer that
// RSTs between the kernel completing the handshake and accept() returning (ECONNABORTED, and
// related per-connection errnos). These must not tear the server down — only a genuinely fatal
// listener error (e.g. EBADF/EINVAL) should. Mirrors the epoll path's is_transient_accept_error().
bool transient_accept_errno() noexcept {
    static constexpr int kTransient[] = {EINTR,     ECONNABORTED, EPROTO,       ENETDOWN,
                                         EHOSTDOWN, ENONET,       EHOSTUNREACH, ENETUNREACH};
    return std::find(std::begin(kTransient), std::end(kTransient), errno) != std::end(kTransient);
}

// accept() one connection, retrying transient errors. Returns a connected fd, or -1 on a genuinely
// fatal listener error (which should stop the accept loop).
int accept_retry(int listen_fd) {
    for (;;) {
        const int conn = ::accept(listen_fd, nullptr, nullptr);
        if (conn >= 0) {
            return conn;
        }
        if (!transient_accept_errno()) {
            return -1;
        }
    }
}

// Test/embedding hook: stop after this many *served* connections (0 = serve indefinitely).
bool reached_accept_limit(std::size_t accepted, std::size_t max_accepts) noexcept {
    return max_accepts != 0 && accepted >= max_accepts;
}

// Spawn a worker thread to serve one connection. The worker closes the descriptor and flags itself
// done (release) so the acceptor can reap and join it (acquire) without blocking on live clients.
void spawn_worker(TcpServer &server, std::vector<ConnectionWorker> &workers, int conn) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    workers.push_back(ConnectionWorker{std::thread([&server, conn, done] {
                                           server.serve_connection(conn);
                                           ::close(conn);
                                           done->store(true, std::memory_order_release);
                                       }),
                                       done});
}

} // namespace

void TcpServer::serve_connection(int fd) {
    Session session(gateway_);
    std::array<std::byte, 4096> buffer{};
    while (!session.logged_out()) {
        const ssize_t n = read_retry(fd, buffer);
        if (n <= 0) {
            break; // EOF (0) or real error (<0): graceful disconnect
        }
        std::vector<std::byte> response;
        SessionStatus status = SessionStatus::Ok;
        {
            std::lock_guard lock(gateway_mutex_);
            status = session.on_bytes(
                std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(n)), response,
                response_limit(options_));
        }
        if (!response.empty() && !write_all(fd, response)) {
            break;
        }
        if (status == SessionStatus::OutputLimitExceeded) {
            break;
        }
    }
}

bool TcpServer::serve_listen_socket(int listen_fd) {
    std::vector<ConnectionWorker> workers;
    std::size_t accepted = 0;
    while (true) {
        const int conn = accept_retry(listen_fd); // retries transient errors; -1 only if fatal
        if (conn < 0) {
            break;
        }
        join_finished(workers); // reap drained workers so the active-connection count is current
        if (at_capacity(workers, options_.max_active_connections)) {
            ::close(conn); // at capacity: shed load rather than spawn an unbounded worker
            continue;
        }
        spawn_worker(*this, workers, conn);
        ++accepted;
        if (reached_accept_limit(accepted, options_.max_accepts)) {
            break;
        }
    }
    join_all(workers);
    return options_.max_accepts == 0 || accepted == options_.max_accepts;
}

bool TcpServer::run(const std::string &host, std::uint16_t port) {
    const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        return false;
    }
    int yes = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, static_cast<socklen_t>(sizeof(yes)));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    const int parsed = ::inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    if (parsed != 1) {
        ::close(listen_fd);
        return false;
    }

    // The sockaddr_in* -> sockaddr* cast is required by the POSIX socket API.
    auto *generic = reinterpret_cast<sockaddr *>(&addr); // NOLINT: POSIX socket API
    if (::bind(listen_fd, generic, static_cast<socklen_t>(sizeof(addr))) < 0 ||
        ::listen(listen_fd, options_.listen_backlog) < 0) {
        ::close(listen_fd);
        return false;
    }
    const bool ok = serve_listen_socket(listen_fd);
    ::close(listen_fd);
    return ok;
}

} // namespace qsl::gateway
