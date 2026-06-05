#include "qsl/gateway/tcp_server.hpp"

#include "qsl/gateway/session.hpp"

#include <arpa/inet.h>
#include <array>
#include <cstddef>
#include <limits>
#include <netinet/in.h>
#include <span>
#include <sys/socket.h>
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
        if (n <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(n);
    }
    return true;
}

} // namespace

void TcpServer::serve_connection(int fd) {
    Session session(gateway_);
    std::array<std::byte, 4096> buffer{};
    while (!session.logged_out()) {
        const ssize_t n = ::read(fd, buffer.data(), buffer.size());
        if (n <= 0) {
            break; // EOF (0) or error (<0): graceful disconnect
        }
        std::vector<std::byte> response;
        const SessionStatus status =
            session.on_bytes(std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(n)),
                             response, response_limit(options_));
        if (!response.empty() && !write_all(fd, response)) {
            break;
        }
        if (status == SessionStatus::OutputLimitExceeded) {
            break;
        }
    }
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
        ::listen(listen_fd, 16) < 0) {
        ::close(listen_fd);
        return false;
    }
    while (true) {
        const int conn = ::accept(listen_fd, nullptr, nullptr);
        if (conn < 0) {
            break;
        }
        serve_connection(conn);
        ::close(conn);
    }
    ::close(listen_fd);
    return true;
}

} // namespace qsl::gateway
