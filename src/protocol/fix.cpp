#include "qsl/protocol/fix.hpp"

#include "qsl/core/types.hpp"

#include <array>
#include <charconv>
#include <string>
#include <string_view>
#include <utility>

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

// Split the message into SOH-delimited tag=value fields. Malformed framing
// (missing SOH, missing '=', non-numeric tag, too many fields) is rejected.
[[nodiscard]] FixError tokenize(std::string_view msg, Parsed &out) noexcept {
    std::size_t pos = 0;
    while (pos < msg.size()) {
        const std::size_t field_start = pos;
        const std::size_t soh = msg.find(kSoh, pos);
        if (soh == std::string_view::npos) {
            return FixError::Malformed; // field not SOH-terminated
        }
        const std::size_t eq = msg.find('=', pos);
        if (eq == std::string_view::npos || eq >= soh) {
            return FixError::Malformed; // no '=' within the field
        }
        unsigned tag = 0;
        if (!parse_int(msg.substr(field_start, eq - field_start), tag)) {
            return FixError::Malformed; // non-numeric tag
        }
        if (out.count >= kMaxFields) {
            return FixError::Malformed; // too many fields
        }
        out.fields[out.count++] = Field{tag, msg.substr(eq + 1, soh - (eq + 1)), field_start};
        pos = soh + 1;
    }
    return FixError::None;
}

// Confirm the 8 / 9 / ... / 10 field ordering and the supported BeginString.
[[nodiscard]] FixError check_envelope_shape(const Parsed &p) noexcept {
    if (p.count < 3) {
        return FixError::Malformed;
    }
    const Field &begin = p.fields[0];
    const bool ordered = begin.tag == kTagBeginString && p.fields[1].tag == kTagBodyLength &&
                         p.fields[p.count - 1].tag == kTagCheckSum;
    if (!ordered) {
        return FixError::Malformed;
    }
    return begin.value == kBeginString ? FixError::None : FixError::UnsupportedBeginString;
}

// Verify BodyLength (tag 9) against the actual body span and the mod-256
// CheckSum (tag 10) against the sum of every byte before the tag-10 field.
[[nodiscard]] FixError verify_length_and_checksum(std::string_view msg, const Parsed &p) noexcept {
    const Field &f_csum = p.fields[p.count - 1];
    // BodyLength spans [fields[2].start, checksum_field.start).
    const std::size_t checksum_start = f_csum.start;
    std::size_t body_len = 0;
    if (!parse_int(p.fields[1].value, body_len)) {
        return FixError::InvalidField;
    }
    if (body_len != checksum_start - p.fields[2].start) {
        return FixError::BodyLengthMismatch;
    }
    unsigned declared = 0;
    if (f_csum.value.size() != 3 || !parse_int(f_csum.value, declared)) {
        return FixError::InvalidField;
    }
    unsigned sum = 0;
    for (std::size_t i = 0; i < checksum_start; ++i) {
        sum += static_cast<unsigned char>(msg[i]);
    }
    return (sum & 0xFFu) == declared ? FixError::None : FixError::ChecksumMismatch;
}

// Validate the FIX envelope and fill the field table; business fields are then
// looked up by the typed decoders.
[[nodiscard]] FixError parse_envelope(std::string_view msg, Parsed &out) noexcept {
    if (msg.empty() || msg.size() > kMaxMessageLen) {
        return FixError::Malformed;
    }
    if (const FixError e = tokenize(msg, out); e != FixError::None) {
        return e;
    }
    if (const FixError e = check_envelope_shape(out); e != FixError::None) {
        return e;
    }
    return verify_length_and_checksum(msg, out);
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

// Confirm MsgType (tag 35) is present, single-character, and the expected type.
[[nodiscard]] FixError expect_msg_type(const Parsed &p, char expected) noexcept {
    const Field *type = find_field(p, kTagMsgType);
    if (type == nullptr || type->value.size() != 1) {
        return FixError::UnknownMsgType;
    }
    return type->value.front() == expected ? FixError::None : FixError::UnknownMsgType;
}

// Reads required fields and short-circuits on the first error, so the typed
// decoders stay a flat chain instead of a long if-return ladder.
class FieldReader {
  public:
    explicit FieldReader(const Parsed &p) noexcept : p_(p) {}

    template <class Int> FieldReader &integer(unsigned tag, Int &out) noexcept {
        if (err_ == FixError::None) {
            err_ = require_int(p_, tag, out);
        }
        return *this;
    }

    // Read a single-character coded field and map it via a {code, enum} table
    // (Side 1/2, OrdType 1/2, TIF 1/3). One generic method covers every enum, so
    // there is no per-enum mapping duplication. An unknown code is InvalidEnumValue.
    template <class Enum, std::size_t N>
    FieldReader &coded(unsigned tag, Enum &out,
                       const std::array<std::pair<char, Enum>, N> &table) noexcept {
        if (err_ != FixError::None) {
            return *this;
        }
        char code = 0;
        err_ = require_code(p_, tag, code);
        if (err_ != FixError::None) {
            return *this;
        }
        for (const auto &entry : table) {
            if (entry.first == code) {
                out = entry.second;
                return *this;
            }
        }
        err_ = FixError::InvalidEnumValue;
        return *this;
    }

    [[nodiscard]] FixError error() const noexcept { return err_; }

  private:
    const Parsed &p_;
    FixError err_{FixError::None};
};

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

// Shared typed-decode skeleton: validate the envelope, confirm MsgType, then let
// `fill` read the body fields through a FieldReader (which short-circuits on the
// first error). Keeps the two public decoders to just their field maps.
template <class T, class Fill>
[[nodiscard]] FixDecodeResult<T> decode_typed(std::string_view msg, char expected,
                                              Fill fill) noexcept {
    Parsed p;
    if (const FixError e = parse_envelope(msg, p); e != FixError::None) {
        return {e, {}};
    }
    if (const FixError e = expect_msg_type(p, expected); e != FixError::None) {
        return {e, {}};
    }
    T out{};
    FieldReader reader(p);
    fill(reader, out);
    if (reader.error() != FixError::None) {
        return {reader.error(), {}};
    }
    return {FixError::None, out};
}

FixDecodeResult<NewOrder> decode_new_order(std::string_view msg) noexcept {
    return decode_typed<NewOrder>(msg, kMsgNewOrderSingle, [](FieldReader &r, NewOrder &o) {
        static constexpr std::array<std::pair<char, Side>, 2> sides{
            {{'1', Side::Buy}, {'2', Side::Sell}}};
        static constexpr std::array<std::pair<char, OrderType>, 2> types{
            {{'1', OrderType::Market}, {'2', OrderType::Limit}}};
        static constexpr std::array<std::pair<char, TimeInForce>, 2> tifs{
            {{'1', TimeInForce::GTC}, {'3', TimeInForce::IOC}}};
        SeqNo seq = 0; // tag 34 (standard header); validated but not stored.
        r.integer(kTagMsgSeqNum, seq)
            .integer(kTagClOrdID, o.order_id)
            .integer(kTagSymbol, o.symbol)
            .integer(kTagOrderQty, o.quantity)
            .integer(kTagPrice, o.price)
            .coded(kTagSide, o.side, sides)
            .coded(kTagOrdType, o.type, types)
            .coded(kTagTimeInForce, o.tif, tifs);
    });
}

FixDecodeResult<CancelOrder> decode_cancel_order(std::string_view msg) noexcept {
    return decode_typed<CancelOrder>(
        msg, kMsgOrderCancelRequest, [](FieldReader &r, CancelOrder &o) {
            SeqNo seq = 0;        // tag 34
            OrderId clord_id = 0; // tag 11 (ClOrdID): required by FIX, validated but not stored.
            r.integer(kTagMsgSeqNum, seq)
                .integer(kTagOrigClOrdID, o.order_id)
                .integer(kTagClOrdID, clord_id)
                .integer(kTagSymbol, o.symbol);
        });
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
