#include "qsl/protocol/fix.hpp"

#include "qsl/core/types.hpp"

#include <array>
#include <charconv>
#include <string>
#include <string_view>

namespace qsl::protocol::fix {

namespace {

// FIX tags this adapter reads/writes.
enum Tag : unsigned {
    kTagBeginString = 8,
    kTagBodyLength = 9,
    kTagCheckSum = 10,
    kTagClOrdID = 11,
    kTagMsgSeqNum = 34,
    kTagMsgType = 35,
    kTagOrderQty = 38,
    kTagOrdType = 40,
    kTagOrigClOrdID = 41,
    kTagPrice = 44,
    kTagSide = 54,
    kTagSymbol = 55,
    kTagTimeInForce = 59,
};

constexpr std::size_t kMaxFields = 32;

struct Field {
    unsigned tag{0};
    std::string_view value{};
    std::size_t start{0}; // byte offset of the field within the message
};

struct Parsed {
    std::array<Field, kMaxFields> fields{};
    std::size_t count{0};
};

// Parse an unsigned/signed integer, requiring the whole view to be consumed.
template <class Int> [[nodiscard]] bool parse_int(std::string_view sv, Int &out) noexcept {
    if (sv.empty()) {
        return false;
    }
    const char *first = sv.data();
    const char *last = sv.data() + sv.size();
    const auto res = std::from_chars(first, last, out);
    return res.ec == std::errc() && res.ptr == last;
}

[[nodiscard]] const Field *find_field(const Parsed &p, unsigned tag) noexcept {
    for (std::size_t i = 0; i < p.count; ++i) {
        if (p.fields[i].tag == tag) {
            return &p.fields[i];
        }
    }
    return nullptr;
}

// Validate the FIX envelope: SOH-delimited tag=value framing, the 8/9/.../10
// ordering, BodyLength (tag 9), and the mod-256 CheckSum (tag 10). On success the
// field table is filled; business fields are looked up by the typed decoders.
[[nodiscard]] FixError parse_envelope(std::string_view msg, Parsed &out) noexcept {
    if (msg.empty() || msg.size() > kMaxMessageLen) {
        return FixError::Malformed;
    }

    std::size_t pos = 0;
    while (pos < msg.size()) {
        const std::size_t field_start = pos;
        const std::size_t soh = msg.find(kSoh, pos);
        if (soh == std::string_view::npos) {
            return FixError::Malformed; // a field is not SOH-terminated
        }
        const std::size_t eq = msg.find('=', pos);
        if (eq == std::string_view::npos || eq >= soh) {
            return FixError::Malformed; // no '=' within the field
        }
        const std::string_view tag_sv = msg.substr(field_start, eq - field_start);
        const std::string_view val_sv = msg.substr(eq + 1, soh - (eq + 1));
        unsigned tag = 0;
        if (!parse_int(tag_sv, tag)) {
            return FixError::Malformed; // non-numeric tag
        }
        if (out.count >= kMaxFields) {
            return FixError::Malformed; // too many fields
        }
        out.fields[out.count++] = Field{tag, val_sv, field_start};
        pos = soh + 1;
    }

    if (out.count < 3) {
        return FixError::Malformed;
    }
    const Field &f_begin = out.fields[0];
    const Field &f_len = out.fields[1];
    const Field &f_csum = out.fields[out.count - 1];
    if (f_begin.tag != kTagBeginString || f_len.tag != kTagBodyLength ||
        f_csum.tag != kTagCheckSum) {
        return FixError::Malformed;
    }
    if (f_begin.value != kBeginString) {
        return FixError::UnsupportedBeginString;
    }

    // BodyLength counts the bytes from the first field after tag 9 through the
    // SOH preceding tag 10, i.e. [fields[2].start, checksum_field.start).
    const std::size_t body_start = out.fields[2].start;
    const std::size_t checksum_start = f_csum.start;
    std::size_t body_len = 0;
    if (!parse_int(f_len.value, body_len)) {
        return FixError::InvalidField;
    }
    if (body_len != checksum_start - body_start) {
        return FixError::BodyLengthMismatch;
    }

    // CheckSum is the mod-256 sum of every byte up to the SOH before tag 10,
    // formatted as exactly three digits.
    unsigned declared = 0;
    if (f_csum.value.size() != 3 || !parse_int(f_csum.value, declared)) {
        return FixError::InvalidField;
    }
    unsigned sum = 0;
    for (std::size_t i = 0; i < checksum_start; ++i) {
        sum += static_cast<unsigned char>(msg[i]);
    }
    if ((sum & 0xFFu) != declared) {
        return FixError::ChecksumMismatch;
    }
    return FixError::None;
}

// Extract a required integer field; map absence/format/overflow to structured
// errors. A value that does not fit the target field is OutOfRange (distinct from
// a non-numeric InvalidField).
template <class Int>
[[nodiscard]] FixError require_int(const Parsed &p, unsigned tag, Int &out) noexcept {
    const Field *f = find_field(p, tag);
    if (f == nullptr) {
        return FixError::MissingField;
    }
    if (f->value.empty()) {
        return FixError::InvalidField;
    }
    const char *first = f->value.data();
    const char *last = f->value.data() + f->value.size();
    const auto res = std::from_chars(first, last, out);
    if (res.ec == std::errc::result_out_of_range) {
        return FixError::OutOfRange;
    }
    if (res.ec != std::errc() || res.ptr != last) {
        return FixError::InvalidField;
    }
    return FixError::None;
}

// Require a single-character coded enum field (e.g. Side, OrdType, TIF).
[[nodiscard]] FixError require_code(const Parsed &p, unsigned tag, char &out) noexcept {
    const Field *f = find_field(p, tag);
    if (f == nullptr) {
        return FixError::MissingField;
    }
    if (f->value.size() != 1) {
        return FixError::InvalidEnumValue;
    }
    out = f->value.front();
    return FixError::None;
}

void append_field(std::string &dst, unsigned tag, std::string_view value) {
    dst += std::to_string(tag);
    dst += '=';
    dst += value;
    dst += kSoh;
}

void append_field(std::string &dst, unsigned tag, std::uint64_t value) {
    append_field(dst, tag, std::to_string(value));
}

// Wrap an already-built body (the fields from tag 35 onward) in the FIX
// envelope: prepend 8/9 and append the computed CheckSum (tag 10).
[[nodiscard]] std::string frame(const std::string &body) {
    std::string head;
    head += "8=";
    head += kBeginString;
    head += kSoh;
    head += "9=";
    head += std::to_string(body.size());
    head += kSoh;
    head += body;

    unsigned sum = 0;
    for (const char c : head) {
        sum += static_cast<unsigned char>(c);
    }
    const unsigned cs = sum % 256u; // FIX CheckSum is the mod-256 byte sum...
    char csum[4];
    // ...formatted as exactly three zero-padded digits (0..255).
    csum[0] = static_cast<char>('0' + ((cs / 100) % 10));
    csum[1] = static_cast<char>('0' + ((cs / 10) % 10));
    csum[2] = static_cast<char>('0' + (cs % 10));
    csum[3] = '\0';

    head += "10=";
    head += csum;
    head += kSoh;
    return head;
}

} // namespace

std::string encode(const NewOrder &msg, SeqNo seq) {
    std::string body;
    append_field(body, kTagMsgType, std::string_view(&kMsgNewOrderSingle, 1));
    append_field(body, kTagMsgSeqNum, seq);
    append_field(body, kTagClOrdID, msg.order_id);
    append_field(body, kTagSymbol, static_cast<std::uint64_t>(msg.symbol));
    append_field(body, kTagSide, msg.side == Side::Buy ? "1" : "2");
    append_field(body, kTagOrderQty, static_cast<std::uint64_t>(msg.quantity));
    append_field(body, kTagOrdType, msg.type == OrderType::Market ? "1" : "2");
    // Price is integer ticks (never a float) and is always present, so the FIX
    // and binary encodings are both lossless bijections over NewOrder.
    append_field(body, kTagPrice, std::to_string(static_cast<long long>(msg.price)));
    append_field(body, kTagTimeInForce, msg.tif == TimeInForce::GTC ? "1" : "3");
    return frame(body);
}

std::string encode(const CancelOrder &msg, SeqNo seq) {
    std::string body;
    append_field(body, kTagMsgType, std::string_view(&kMsgOrderCancelRequest, 1));
    append_field(body, kTagMsgSeqNum, seq);
    // OrigClOrdID identifies the order to cancel; ClOrdID is the (required) id of
    // the cancel request itself. CancelOrder carries only one id, so both echo it.
    append_field(body, kTagOrigClOrdID, msg.order_id);
    append_field(body, kTagClOrdID, msg.order_id);
    append_field(body, kTagSymbol, static_cast<std::uint64_t>(msg.symbol));
    return frame(body);
}

FixDecodeResult<NewOrder> decode_new_order(std::string_view msg) noexcept {
    Parsed p;
    if (const FixError e = parse_envelope(msg, p); e != FixError::None) {
        return {e, {}};
    }
    const Field *type = find_field(p, kTagMsgType);
    if (type == nullptr || type->value.size() != 1 || type->value.front() != kMsgNewOrderSingle) {
        return {FixError::UnknownMsgType, {}};
    }

    NewOrder out{};
    SeqNo seq = 0; // standard header field (tag 34); validated but not stored.
    if (const FixError e = require_int(p, kTagMsgSeqNum, seq); e != FixError::None) {
        return {e, {}};
    }
    if (const FixError e = require_int(p, kTagClOrdID, out.order_id); e != FixError::None) {
        return {e, {}};
    }
    if (const FixError e = require_int(p, kTagSymbol, out.symbol); e != FixError::None) {
        return {e, {}};
    }
    if (const FixError e = require_int(p, kTagOrderQty, out.quantity); e != FixError::None) {
        return {e, {}};
    }
    if (const FixError e = require_int(p, kTagPrice, out.price); e != FixError::None) {
        return {e, {}};
    }

    char side = 0;
    char ord_type = 0;
    char tif = 0;
    if (const FixError e = require_code(p, kTagSide, side); e != FixError::None) {
        return {e, {}};
    }
    if (const FixError e = require_code(p, kTagOrdType, ord_type); e != FixError::None) {
        return {e, {}};
    }
    if (const FixError e = require_code(p, kTagTimeInForce, tif); e != FixError::None) {
        return {e, {}};
    }

    switch (side) {
    case '1':
        out.side = Side::Buy;
        break;
    case '2':
        out.side = Side::Sell;
        break;
    default:
        return {FixError::InvalidEnumValue, {}};
    }
    switch (ord_type) {
    case '1':
        out.type = OrderType::Market;
        break;
    case '2':
        out.type = OrderType::Limit;
        break;
    default:
        return {FixError::InvalidEnumValue, {}};
    }
    switch (tif) {
    case '1':
        out.tif = TimeInForce::GTC;
        break;
    case '3':
        out.tif = TimeInForce::IOC;
        break;
    default:
        return {FixError::InvalidEnumValue, {}};
    }
    return {FixError::None, out};
}

FixDecodeResult<CancelOrder> decode_cancel_order(std::string_view msg) noexcept {
    Parsed p;
    if (const FixError e = parse_envelope(msg, p); e != FixError::None) {
        return {e, {}};
    }
    const Field *type = find_field(p, kTagMsgType);
    if (type == nullptr || type->value.size() != 1 ||
        type->value.front() != kMsgOrderCancelRequest) {
        return {FixError::UnknownMsgType, {}};
    }

    CancelOrder out{};
    SeqNo seq = 0;
    if (const FixError e = require_int(p, kTagMsgSeqNum, seq); e != FixError::None) {
        return {e, {}};
    }
    if (const FixError e = require_int(p, kTagOrigClOrdID, out.order_id); e != FixError::None) {
        return {e, {}};
    }
    if (const FixError e = require_int(p, kTagSymbol, out.symbol); e != FixError::None) {
        return {e, {}};
    }
    return {FixError::None, out};
}

FixDecodeResult<char> peek_msg_type(std::string_view msg) noexcept {
    Parsed p;
    if (const FixError e = parse_envelope(msg, p); e != FixError::None) {
        return {e, {}};
    }
    const Field *type = find_field(p, kTagMsgType);
    if (type == nullptr || type->value.size() != 1) {
        return {FixError::UnknownMsgType, {}};
    }
    return {FixError::None, type->value.front()};
}

FixDecodeResult<SeqNo> peek_seq(std::string_view msg) noexcept {
    Parsed p;
    if (const FixError e = parse_envelope(msg, p); e != FixError::None) {
        return {e, {}};
    }
    SeqNo seq = 0;
    if (const FixError e = require_int(p, kTagMsgSeqNum, seq); e != FixError::None) {
        return {e, {}};
    }
    return {FixError::None, seq};
}

} // namespace qsl::protocol::fix
