#include "qsl/feed/udp_feed.hpp"

#include <arpa/inet.h>
#include <array>
#include <cstddef>
#include <netinet/in.h>
#include <span>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <variant>

namespace qsl::feed {

UdpPublisher::UdpPublisher(const std::string &host, std::uint16_t port) {
    in_addr parsed{};
    if (::inet_pton(AF_INET, host.c_str(), &parsed) != 1) {
        return; // invalid host -> fd_ stays -1, good() is false
    }
    dest_addr_ = parsed.s_addr;
    dest_port_ = htons(port);
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
}

UdpPublisher::~UdpPublisher() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

void UdpPublisher::on_market_data(const MarketDataMessage &msg) {
    if (fd_ < 0) {
        return;
    }
    const std::vector<std::byte> bytes = std::visit([](const auto &m) { return encode(m); }, msg);
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = dest_port_;
    dest.sin_addr.s_addr = dest_addr_;
    const auto *generic = reinterpret_cast<const sockaddr *>(&dest); // NOLINT: POSIX socket API
    const ssize_t sent =
        ::sendto(fd_, bytes.data(), bytes.size(), 0, generic, static_cast<socklen_t>(sizeof(dest)));
    if (sent < 0 || static_cast<std::size_t>(sent) != bytes.size()) {
        ++send_failures_; // best-effort feed: record the loss rather than discard it silently
    }
}

UdpFeedClient::UdpFeedClient(std::uint16_t port, int recv_buffer_bytes) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        return;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    auto *generic = reinterpret_cast<sockaddr *>(&addr); // NOLINT: POSIX socket API
    if (::bind(fd_, generic, static_cast<socklen_t>(sizeof(addr))) < 0) {
        ::close(fd_);
        fd_ = -1;
        return;
    }
    // Bounded receive timeout so a subscriber waiting for missing datagrams does not hang
    // forever; recvfrom then fails with EAGAIN and receive() returns std::nullopt.
    timeval timeout{};
    timeout.tv_sec = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, static_cast<socklen_t>(sizeof(timeout)));
    // Optional receive-buffer sizing for the M30 socket-buffer experiment. A larger SO_RCVBUF
    // lets the kernel queue more datagrams during a burst before dropping them; a small one
    // makes loss (and the resulting sequence gaps) easy to observe on loopback. The kernel may
    // round up or clamp the request, so the granted value is read back for honest reporting.
    if (recv_buffer_bytes > 0) {
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &recv_buffer_bytes,
                     static_cast<socklen_t>(sizeof(recv_buffer_bytes)));
    }
    int effective_buffer = 0;
    socklen_t effective_len = sizeof(effective_buffer);
    if (::getsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &effective_buffer, &effective_len) == 0) {
        recv_buffer_bytes_ = effective_buffer;
    }
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    auto *bound_generic = reinterpret_cast<sockaddr *>(&bound); // NOLINT: POSIX socket API
    if (::getsockname(fd_, bound_generic, &len) == 0) {
        port_ = ntohs(bound.sin_port);
    }
}

UdpFeedClient::~UdpFeedClient() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

std::optional<MarketDataMessage> UdpFeedClient::receive(std::uint64_t &gap) {
    gap = 0;
    if (fd_ < 0) {
        return std::nullopt;
    }
    std::array<std::byte, 1024> buffer{};
    const ssize_t n = ::recvfrom(fd_, buffer.data(), buffer.size(), 0, nullptr, nullptr);
    if (n <= 0) {
        return std::nullopt;
    }
    auto message =
        decode_market_data(std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(n)));
    if (!message) {
        return std::nullopt;
    }
    gap = tracker_.observe(md_seq_of(*message));
    total_gaps_ += gap;
    return message;
}

} // namespace qsl::feed
