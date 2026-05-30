#include "qsl/protocol/codec.hpp"

#include <arpa/inet.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <netinet/in.h>
#include <span>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

bool write_all(int fd, const std::vector<std::byte> &data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t n = ::write(fd, data.data() + offset, data.size() - offset);
        if (n <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(n);
    }
    return true;
}

void print_responses(std::span<const std::byte> bytes) {
    namespace p = qsl::protocol;
    std::size_t offset = 0;
    while (offset + p::kHeaderSize <= bytes.size()) {
        const auto frame = bytes.subspan(offset);
        const auto header = p::decode_header(frame);
        if (header.error != p::DecodeError::None) {
            std::cout << "  <malformed response>\n";
            return;
        }
        const std::size_t total = p::kHeaderSize + header.value.body_len;
        if (offset + total > bytes.size()) {
            return;
        }
        const auto one = frame.subspan(0, total);
        switch (header.value.type) {
        case p::MsgType::Ack:
            std::cout << "  Ack order=" << p::decode_ack(one).value.order_id
                      << " seq=" << p::decode_ack(one).value.seq << "\n";
            break;
        case p::MsgType::Reject:
            std::cout << "  Reject order=" << p::decode_reject(one).value.order_id
                      << " reason=" << qsl::core::to_string(p::decode_reject(one).value.reason)
                      << "\n";
            break;
        case p::MsgType::Fill: {
            const auto f = p::decode_fill(one).value;
            std::cout << "  Fill taker=" << f.taker_id << " maker=" << f.maker_id
                      << " price=" << f.price << " qty=" << f.quantity << "\n";
            break;
        }
        case p::MsgType::HeartbeatAck:
            std::cout << "  HeartbeatAck token=" << p::decode_heartbeat_ack(one).value.token
                      << "\n";
            break;
        default:
            std::cout << "  <unexpected response>\n";
            break;
        }
        offset += total;
    }
}

} // namespace

// qsl-client [port]  -> connect to 127.0.0.1:port, send a NewOrder and a Heartbeat, print replies.
int main(int argc, char **argv) {
    const std::uint16_t port = (argc >= 2) ? static_cast<std::uint16_t>(std::stoul(argv[1])) : 9009;

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "socket failed\n";
        return 1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    auto *generic = reinterpret_cast<sockaddr *>(&addr); // NOLINT: POSIX socket API
    if (::connect(fd, generic, static_cast<socklen_t>(sizeof(addr))) < 0) {
        std::cerr << "connect to 127.0.0.1:" << port << " failed\n";
        ::close(fd);
        return 1;
    }

    const qsl::protocol::NewOrder order{/*order_id=*/1,
                                        /*symbol=*/0,
                                        /*price=*/100,
                                        /*quantity=*/5,
                                        qsl::core::Side::Buy,
                                        qsl::core::OrderType::Limit,
                                        qsl::core::TimeInForce::GTC};
    write_all(fd, qsl::protocol::encode(order, /*seq=*/1));
    write_all(fd, qsl::protocol::encode(qsl::protocol::Heartbeat{/*token=*/42}));
    ::shutdown(fd, SHUT_WR); // signal end-of-requests; server replies then closes

    std::vector<std::byte> received;
    std::array<std::byte, 4096> buf{};
    while (true) {
        const ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n <= 0) {
            break;
        }
        received.insert(received.end(), buf.begin(), buf.begin() + n);
    }
    std::cout << "responses:\n";
    print_responses(received);
    ::close(fd);
    return 0;
}
