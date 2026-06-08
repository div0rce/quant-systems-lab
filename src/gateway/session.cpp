#include "qsl/gateway/session.hpp"

#include "qsl/engine/events.hpp"
#include "qsl/protocol/codec.hpp"

#include <algorithm>
#include <limits>
#include <variant>

namespace qsl::gateway {
namespace {

constexpr std::size_t frame_bytes(std::size_t body_bytes) noexcept {
    return protocol::kHeaderSize + body_bytes;
}

inline constexpr std::size_t kHeartbeatAckFrameBytes = frame_bytes(protocol::kHeartbeatBodySize);
inline constexpr std::size_t kAckFrameBytes = frame_bytes(protocol::kAckBodySize);
inline constexpr std::size_t kRejectFrameBytes = frame_bytes(protocol::kRejectBodySize);
inline constexpr std::size_t kFillFrameBytes = frame_bytes(protocol::kFillBodySize);

bool has_space(std::size_t current, std::size_t add, std::size_t limit) noexcept {
    return current <= limit && add <= limit - current;
}

std::size_t add_saturating(std::size_t a, std::size_t b) noexcept {
    const std::size_t max = std::numeric_limits<std::size_t>::max();
    return b > max - a ? max : a + b;
}

std::size_t mul_saturating(std::size_t a, std::size_t b) noexcept {
    const std::size_t max = std::numeric_limits<std::size_t>::max();
    return a != 0 && b > max / a ? max : a * b;
}

std::size_t result_wire_size(const GatewayResult &result) noexcept {
    if (!result.accepted) {
        return kRejectFrameBytes;
    }
    std::size_t size = kAckFrameBytes;
    for (const auto &event : result.events) {
        if (std::holds_alternative<engine::TradeEvent>(event)) {
            size = add_saturating(size, kFillFrameBytes);
        }
    }
    return size;
}

SessionStatus append(std::vector<std::byte> &out, const std::vector<std::byte> &bytes,
                     std::size_t max_output_bytes) {
    if (!has_space(out.size(), bytes.size(), max_output_bytes)) {
        return SessionStatus::OutputLimitExceeded;
    }
    out.insert(out.end(), bytes.begin(), bytes.end());
    return SessionStatus::Ok;
}

// Translate a gateway result into wire responses: a Reject, or an Ack followed by a Fill
// per trade.
SessionStatus emit_result(core::OrderId order_id, const GatewayResult &result,
                          std::vector<std::byte> &out, std::size_t max_output_bytes) {
    if (!has_space(out.size(), result_wire_size(result), max_output_bytes)) {
        return SessionStatus::OutputLimitExceeded;
    }
    if (!result.accepted) {
        return append(out, protocol::encode(protocol::Reject{order_id, result.reason}),
                      max_output_bytes);
    }
    const core::SeqNo ack_seq = result.events.empty() ? 0 : engine::seq_of(result.events.front());
    if (append(out, protocol::encode(protocol::Ack{order_id, ack_seq}), max_output_bytes) !=
        SessionStatus::Ok) {
        return SessionStatus::OutputLimitExceeded;
    }
    for (const auto &event : result.events) {
        if (const auto *t = std::get_if<engine::TradeEvent>(&event)) {
            if (append(out,
                       protocol::encode(
                           protocol::Fill{t->taker_id, t->maker_id, t->price, t->quantity, t->seq}),
                       max_output_bytes) != SessionStatus::Ok) {
                return SessionStatus::OutputLimitExceeded;
            }
        }
    }
    return SessionStatus::Ok;
}

SessionStatus ensure_new_order_budget(const OrderGateway &gateway, const protocol::NewOrder &order,
                                      std::vector<std::byte> &out, std::size_t max_output_bytes) {
    const NewOrderPreview preview =
        gateway.preview_new_order(order.symbol, order.order_id, order.side, order.price,
                                  order.quantity, order.type, order.tif);
    const std::size_t required =
        preview.accepted
            ? add_saturating(kAckFrameBytes, mul_saturating(preview.fill_count, kFillFrameBytes))
            : kRejectFrameBytes;
    return has_space(out.size(), required, max_output_bytes) ? SessionStatus::Ok
                                                             : SessionStatus::OutputLimitExceeded;
}

struct FrameContext {
    OrderGateway &gateway;
    bool &logged_out;
    std::vector<std::byte> &out;
    std::size_t max_output_bytes;
};

SessionStatus close_session(FrameContext &ctx, SessionStatus status) noexcept {
    ctx.logged_out = true;
    return status;
}

SessionStatus close_for_malformed_body(FrameContext &ctx) noexcept {
    return close_session(ctx, SessionStatus::Ok);
}

SessionStatus close_for_output_limit(FrameContext &ctx) noexcept {
    return close_session(ctx, SessionStatus::OutputLimitExceeded);
}

SessionStatus emit_or_close(FrameContext &ctx, core::OrderId order_id,
                            const GatewayResult &result) {
    return emit_result(order_id, result, ctx.out, ctx.max_output_bytes) == SessionStatus::Ok
               ? SessionStatus::Ok
               : close_for_output_limit(ctx);
}

SessionStatus handle_new_order(FrameContext &ctx, std::span<const std::byte> frame) {
    const auto request = protocol::decode_new_order(frame);
    if (!request.ok()) {
        return close_for_malformed_body(ctx);
    }
    if (ctx.max_output_bytes != std::numeric_limits<std::size_t>::max() &&
        ensure_new_order_budget(ctx.gateway, request.value, ctx.out, ctx.max_output_bytes) !=
            SessionStatus::Ok) {
        return close_for_output_limit(ctx);
    }
    const auto &o = request.value;
    const GatewayResult result =
        (o.type == core::OrderType::Market)
            ? ctx.gateway.new_market(o.symbol, o.order_id, o.side, o.quantity)
            : ctx.gateway.new_limit(o.symbol, o.order_id, o.side, o.price, o.quantity, o.tif);
    return emit_or_close(ctx, o.order_id, result);
}

SessionStatus handle_cancel_order(FrameContext &ctx, std::span<const std::byte> frame) {
    const auto request = protocol::decode_cancel_order(frame);
    if (!request.ok()) {
        return close_for_malformed_body(ctx);
    }
    if (!has_space(ctx.out.size(), std::max(kAckFrameBytes, kRejectFrameBytes),
                   ctx.max_output_bytes)) {
        return close_for_output_limit(ctx);
    }
    const GatewayResult result = ctx.gateway.cancel(request.value.symbol, request.value.order_id);
    return emit_or_close(ctx, request.value.order_id, result);
}

SessionStatus handle_heartbeat(FrameContext &ctx, std::span<const std::byte> frame) {
    const auto request = protocol::decode_heartbeat(frame);
    if (!request.ok()) {
        return close_for_malformed_body(ctx);
    }
    if (!has_space(ctx.out.size(), kHeartbeatAckFrameBytes, ctx.max_output_bytes)) {
        return close_for_output_limit(ctx);
    }
    return append(ctx.out, protocol::encode(protocol::HeartbeatAck{request.value.token}),
                  ctx.max_output_bytes);
}

SessionStatus handle_unexpected_message(FrameContext &ctx) noexcept {
    return close_session(ctx, SessionStatus::Ok);
}

} // namespace

SessionStatus Session::process_frame(std::span<const std::byte> frame, std::vector<std::byte> &out,
                                     std::size_t max_output_bytes) {
    const auto header = protocol::decode_header(frame); // already validated by on_bytes
    FrameContext ctx{gateway_, logged_out_, out, max_output_bytes};
    switch (header.value.type) {
    case protocol::MsgType::NewOrder:
        return handle_new_order(ctx, frame);
    case protocol::MsgType::CancelOrder:
        return handle_cancel_order(ctx, frame);
    case protocol::MsgType::Heartbeat:
        return handle_heartbeat(ctx, frame);
    default:
        return handle_unexpected_message(ctx); // unexpected (e.g. a response) message -> drop
    }
}

SessionStatus Session::on_bytes(std::span<const std::byte> input, std::vector<std::byte> &out,
                                std::size_t max_output_bytes) {
    if (logged_out_) {
        return SessionStatus::Ok;
    }
    inbuf_.insert(inbuf_.end(), input.begin(), input.end());
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
        const SessionStatus status =
            process_frame(remaining.subspan(0, total), out, max_output_bytes);
        if (status != SessionStatus::Ok) {
            inbuf_.erase(inbuf_.begin(), inbuf_.begin() + static_cast<std::ptrdiff_t>(consumed));
            return status;
        }
        consumed += total;
    }
    inbuf_.erase(inbuf_.begin(), inbuf_.begin() + static_cast<std::ptrdiff_t>(consumed));
    return SessionStatus::Ok;
}

std::vector<std::byte> Session::on_bytes(std::span<const std::byte> input) {
    std::vector<std::byte> out;
    static_cast<void>(on_bytes(input, out, std::numeric_limits<std::size_t>::max()));
    return out;
}

} // namespace qsl::gateway
