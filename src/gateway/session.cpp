#include "qsl/gateway/session.hpp"

#include "qsl/engine/events.hpp"
#include "qsl/protocol/codec.hpp"

#include <variant>

namespace qsl::gateway {
namespace {

void append(std::vector<std::byte> &out, const std::vector<std::byte> &bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

// Translate a gateway result into wire responses: a Reject, or an Ack followed by a Fill
// per trade.
void emit_result(core::OrderId order_id, const GatewayResult &result, std::vector<std::byte> &out) {
    if (!result.accepted) {
        append(out, protocol::encode(protocol::Reject{order_id, result.reason}));
        return;
    }
    const core::SeqNo ack_seq = result.events.empty() ? 0 : engine::seq_of(result.events.front());
    append(out, protocol::encode(protocol::Ack{order_id, ack_seq}));
    for (const auto &event : result.events) {
        if (const auto *t = std::get_if<engine::TradeEvent>(&event)) {
            append(out, protocol::encode(protocol::Fill{t->taker_id, t->maker_id, t->price,
                                                        t->quantity, t->seq}));
        }
    }
}

} // namespace

void Session::process_frame(std::span<const std::byte> frame, std::vector<std::byte> &out) {
    const auto header = protocol::decode_header(frame); // already validated by on_bytes
    switch (header.value.type) {
    case protocol::MsgType::NewOrder: {
        const auto request = protocol::decode_new_order(frame);
        if (!request.ok()) {
            logged_out_ = true;
            return;
        }
        const auto &o = request.value;
        const GatewayResult result =
            (o.type == core::OrderType::Market)
                ? gateway_.new_market(o.symbol, o.order_id, o.side, o.quantity)
                : gateway_.new_limit(o.symbol, o.order_id, o.side, o.price, o.quantity, o.tif);
        emit_result(o.order_id, result, out);
        return;
    }
    case protocol::MsgType::CancelOrder: {
        const auto request = protocol::decode_cancel_order(frame);
        if (!request.ok()) {
            logged_out_ = true;
            return;
        }
        emit_result(request.value.order_id,
                    gateway_.cancel(request.value.symbol, request.value.order_id), out);
        return;
    }
    case protocol::MsgType::Heartbeat: {
        const auto request = protocol::decode_heartbeat(frame);
        if (!request.ok()) {
            logged_out_ = true;
            return;
        }
        append(out, protocol::encode(protocol::HeartbeatAck{request.value.token}));
        return;
    }
    default:
        logged_out_ = true; // unexpected (e.g. a response) message -> drop
        return;
    }
}

std::vector<std::byte> Session::on_bytes(std::span<const std::byte> input) {
    inbuf_.insert(inbuf_.end(), input.begin(), input.end());
    std::vector<std::byte> out;
    std::size_t consumed = 0;
    while (!logged_out_) {
        const std::span<const std::byte> remaining(inbuf_.data() + consumed,
                                                   inbuf_.size() - consumed);
        if (remaining.size() < protocol::kHeaderSize) {
            break; // need more bytes for a header
        }
        const auto header = protocol::decode_header(remaining);
        if (header.error != protocol::DecodeError::None) {
            logged_out_ = true; // malformed header: cannot reframe safely -> drop
            break;
        }
        const std::size_t total = protocol::kHeaderSize + header.value.body_len;
        if (remaining.size() < total) {
            break; // wait for the full body
        }
        process_frame(remaining.subspan(0, total), out);
        consumed += total;
    }
    inbuf_.erase(inbuf_.begin(), inbuf_.begin() + static_cast<std::ptrdiff_t>(consumed));
    return out;
}

} // namespace qsl::gateway
