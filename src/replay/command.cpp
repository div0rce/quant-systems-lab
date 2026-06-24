#include "qsl/replay/command.hpp"

#include "qsl/protocol/endian.hpp"

#include <cstdint>

namespace qsl::replay {
namespace {

enum class Tag : std::uint8_t {
    RegisterSymbol = 0,
    NewLimit = 1,
    NewMarket = 2,
    Cancel = 3,
    Modify = 4,
};

constexpr std::size_t kNewLimitSize = 28;  // tag + 4+8+8+4+1+1+1
constexpr std::size_t kNewMarketSize = 18; // tag + 4+8+1+4 (+1 pad? no) -> 4+8+1+4=17, +tag=18
constexpr std::size_t kCancelSize = 13;    // tag + 4+8
constexpr std::size_t kModifySize = 25;    // tag + 4+8+8+4

std::byte tag_byte(Tag t) noexcept {
    return static_cast<std::byte>(static_cast<std::uint8_t>(t));
}

} // namespace

std::vector<std::byte> encode_command(const Command &command) {
    std::vector<std::byte> out;
    if (const auto *c = std::get_if<RegisterSymbol>(&command)) {
        out.reserve(1 + c->name.size());
        out.push_back(tag_byte(Tag::RegisterSymbol));
        for (const char ch : c->name) {
            out.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
        }
        return out;
    }
    if (const auto *c = std::get_if<NewLimit>(&command)) {
        out.resize(kNewLimitSize);
        out[0] = tag_byte(Tag::NewLimit);
        protocol::store_be<std::uint32_t>(out.data() + 1, c->symbol);
        protocol::store_be<std::uint64_t>(out.data() + 5, c->id);
        protocol::store_be<std::uint64_t>(out.data() + 13, static_cast<std::uint64_t>(c->price));
        protocol::store_be<std::uint32_t>(out.data() + 21, c->quantity);
        out[25] = static_cast<std::byte>(static_cast<std::uint8_t>(c->side));
        out[26] = static_cast<std::byte>(static_cast<std::uint8_t>(c->tif));
        return out;
    }
    if (const auto *c = std::get_if<NewMarket>(&command)) {
        out.resize(kNewMarketSize);
        out[0] = tag_byte(Tag::NewMarket);
        protocol::store_be<std::uint32_t>(out.data() + 1, c->symbol);
        protocol::store_be<std::uint64_t>(out.data() + 5, c->id);
        out[13] = static_cast<std::byte>(static_cast<std::uint8_t>(c->side));
        protocol::store_be<std::uint32_t>(out.data() + 14, c->quantity);
        return out;
    }
    if (const auto *c = std::get_if<Cancel>(&command)) {
        out.resize(kCancelSize);
        out[0] = tag_byte(Tag::Cancel);
        protocol::store_be<std::uint32_t>(out.data() + 1, c->symbol);
        protocol::store_be<std::uint64_t>(out.data() + 5, c->id);
        return out;
    }
    const auto &c = std::get<Modify>(command);
    out.resize(kModifySize);
    out[0] = tag_byte(Tag::Modify);
    protocol::store_be<std::uint32_t>(out.data() + 1, c.symbol);
    protocol::store_be<std::uint64_t>(out.data() + 5, c.id);
    protocol::store_be<std::uint64_t>(out.data() + 13, static_cast<std::uint64_t>(c.price));
    protocol::store_be<std::uint32_t>(out.data() + 21, c.quantity);
    return out;
}

namespace {

// Per-tag decoders. Each validates length first, then any enum domains. Out-of-domain enum bytes
// are refused (return nullopt): the replay path applies decoded commands straight to the engine (no
// gateway risk check), so a corrupt Side/TIF would otherwise diverge replayed state instead of
// being rejected like a malformed frame.
std::optional<Command> decode_register_symbol(std::span<const std::byte> bytes) {
    std::string name;
    for (std::size_t i = 1; i < bytes.size(); ++i) {
        name.push_back(static_cast<char>(std::to_integer<unsigned char>(bytes[i])));
    }
    return RegisterSymbol{std::move(name)};
}

std::optional<Command> decode_new_limit(std::span<const std::byte> bytes) {
    if (bytes.size() < kNewLimitSize) {
        return std::nullopt;
    }
    const std::byte *p = bytes.data();
    const auto side = static_cast<Side>(std::to_integer<std::uint8_t>(p[25]));
    const auto tif = static_cast<TimeInForce>(std::to_integer<std::uint8_t>(p[26]));
    if (!core::is_valid(side) || !core::is_valid(tif)) {
        return std::nullopt;
    }
    return NewLimit{protocol::load_be<std::uint32_t>(p + 1),
                    protocol::load_be<std::uint64_t>(p + 5),
                    side,
                    static_cast<Price>(protocol::load_be<std::uint64_t>(p + 13)),
                    protocol::load_be<std::uint32_t>(p + 21),
                    tif};
}

std::optional<Command> decode_new_market(std::span<const std::byte> bytes) {
    if (bytes.size() < kNewMarketSize) {
        return std::nullopt;
    }
    const std::byte *p = bytes.data();
    const auto side = static_cast<Side>(std::to_integer<std::uint8_t>(p[13]));
    if (!core::is_valid(side)) {
        return std::nullopt;
    }
    return NewMarket{protocol::load_be<std::uint32_t>(p + 1),
                     protocol::load_be<std::uint64_t>(p + 5), side,
                     protocol::load_be<std::uint32_t>(p + 14)};
}

std::optional<Command> decode_cancel(std::span<const std::byte> bytes) {
    if (bytes.size() < kCancelSize) {
        return std::nullopt;
    }
    const std::byte *p = bytes.data();
    return Cancel{protocol::load_be<std::uint32_t>(p + 1), protocol::load_be<std::uint64_t>(p + 5)};
}

std::optional<Command> decode_modify(std::span<const std::byte> bytes) {
    if (bytes.size() < kModifySize) {
        return std::nullopt;
    }
    const std::byte *p = bytes.data();
    return Modify{protocol::load_be<std::uint32_t>(p + 1), protocol::load_be<std::uint64_t>(p + 5),
                  static_cast<Price>(protocol::load_be<std::uint64_t>(p + 13)),
                  protocol::load_be<std::uint32_t>(p + 21)};
}

} // namespace

std::optional<Command> decode_command(std::span<const std::byte> bytes) {
    if (bytes.empty()) {
        return std::nullopt;
    }
    switch (static_cast<Tag>(std::to_integer<std::uint8_t>(bytes[0]))) {
    case Tag::RegisterSymbol:
        return decode_register_symbol(bytes);
    case Tag::NewLimit:
        return decode_new_limit(bytes);
    case Tag::NewMarket:
        return decode_new_market(bytes);
    case Tag::Cancel:
        return decode_cancel(bytes);
    case Tag::Modify:
        return decode_modify(bytes);
    }
    return std::nullopt;
}

} // namespace qsl::replay
