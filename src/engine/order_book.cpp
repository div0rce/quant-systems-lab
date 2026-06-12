#include "qsl/engine/order_book.hpp"

#include "qsl/memory/order_pool.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <new>
#include <utility>

namespace qsl::engine {

namespace {

constexpr std::size_t kIntrusivePoolCapacity = 65'536;

template <class T, std::size_t Capacity> class RawPool {
  public:
    RawPool() noexcept { reset_free_list(); }
    ~RawPool() noexcept { destroy_live(); }

    RawPool(const RawPool &) = delete;
    RawPool &operator=(const RawPool &) = delete;

    template <class... Args> [[nodiscard]] T *try_acquire(Args &&...args) {
        if (free_count_ == 0) {
            return nullptr;
        }
        const std::size_t idx = free_indices_[free_count_ - 1];
        auto *object = reinterpret_cast<T *>(slot_storage(idx));
        std::construct_at(object, std::forward<Args>(args)...);
        --free_count_;
        live_[idx] = true;
        return slot_ptr(idx);
    }

    bool release(T *object) noexcept {
        std::size_t slot = 0;
        if (!slot_index(object, slot) || !live_[slot]) {
            return false;
        }
        std::destroy_at(slot_ptr(slot));
        live_[slot] = false;
        free_indices_[free_count_++] = slot;
        return true;
    }

    [[nodiscard]] std::size_t available() const noexcept { return free_count_; }

  private:
    void reset_free_list() noexcept {
        free_count_ = Capacity;
        for (std::size_t i = 0; i < Capacity; ++i) {
            free_indices_[i] = Capacity - 1 - i;
        }
    }

    [[nodiscard]] T *slot_ptr(std::size_t idx) noexcept {
        return std::launder(reinterpret_cast<T *>(slot_storage(idx)));
    }

    [[nodiscard]] std::byte *slot_storage(std::size_t idx) noexcept {
        return storage_.data() + (idx * sizeof(T));
    }

    [[nodiscard]] bool slot_index(const T *object, std::size_t &slot) const noexcept {
        if (object == nullptr) {
            return false;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(storage_.data());
        const auto raw = reinterpret_cast<std::uintptr_t>(object);
        constexpr std::size_t width = sizeof(T);
        constexpr std::size_t alignment = alignof(T);
        if (raw < base || raw >= base + (Capacity * width)) {
            return false;
        }
        const std::uintptr_t offset = raw - base;
        if (offset % width != 0 || raw % alignment != 0) {
            return false;
        }
        slot = static_cast<std::size_t>(offset / width);
        return slot < Capacity;
    }

    void destroy_live() noexcept {
        for (std::size_t i = 0; i < Capacity; ++i) {
            if (live_[i]) {
                std::destroy_at(slot_ptr(i));
                live_[i] = false;
            }
        }
    }

    alignas(T) std::array<std::byte, sizeof(T) * Capacity> storage_{};
    std::array<bool, Capacity> live_{};
    std::array<std::size_t, Capacity> free_indices_{};
    std::size_t free_count_{0};
};

} // namespace

template <class ContainsFn, class MatchFn>
OrderBook::MatchResult OrderBook::match_incoming(OrderId id, Side side, Price limit, bool is_market,
                                                 Quantity quantity, ContainsFn &&contains_fn,
                                                 MatchFn &&match_fn) {
    MatchResult result{{}, quantity, false};
    if (contains_fn()) {
        return result;
    }
    result.accepted = true;
    MatchContext ctx{id, side == Side::Buy, limit, is_market, result.remainder, result.trades};
    std::forward<MatchFn>(match_fn)(side, ctx);
    return result;
}

bool OrderBook::fill_maker(MatchContext &ctx, OrderId maker_id, Quantity &maker_quantity,
                           Price level_price) {
    const Quantity traded = std::min(ctx.quantity, maker_quantity);
    ctx.trades.push_back(Trade{ctx.taker_id, maker_id, level_price, traded});
    ctx.quantity -= traded;
    maker_quantity -= traded;
    return maker_quantity == 0;
}

struct OrderBook::IntrusiveStore {
    struct Node {
        Order *order;
        Node *prev;
        Node *next;
    };

    struct Level {
        Node *head = nullptr;
        Node *tail = nullptr;
        std::size_t size = 0;
    };

    struct Locator {
        Side side;
        Price price;
        Node *node;
    };

    struct LimitInput {
        OrderId id;
        Side side;
        Price price;
        Quantity quantity;
        TimeInForce tif;
    };

    using BidMap = std::map<Price, Level, std::greater<Price>>;
    using AskMap = std::map<Price, Level, std::less<Price>>;

    memory::OrderPool<kIntrusivePoolCapacity> orders;
    RawPool<Node, kIntrusivePoolCapacity> nodes;
    BidMap bids;
    AskMap asks;
    std::unordered_map<OrderId, Locator> index;

    [[nodiscard]] bool has_capacity() const noexcept {
        return orders.available() > 0 && nodes.available() > 0;
    }

    static void append_node(Level &level, Node *node) noexcept {
        node->prev = level.tail;
        node->next = nullptr;
        if (level.tail != nullptr) {
            level.tail->next = node;
        } else {
            level.head = node;
        }
        level.tail = node;
        ++level.size;
    }

    static void unlink_node(Level &level, Node *node) noexcept {
        if (node->prev != nullptr) {
            node->prev->next = node->next;
        } else {
            level.head = node->next;
        }
        if (node->next != nullptr) {
            node->next->prev = node->prev;
        } else {
            level.tail = node->prev;
        }
        --level.size;
    }

    void destroy_node(Node *node) noexcept {
        static_cast<void>(orders.release(node->order));
        static_cast<void>(nodes.release(node));
    }

    template <class LevelMap> Level &level_for(LevelMap &levels, Price price) {
        return levels.try_emplace(price).first->second;
    }

    Level &level_for(Side side, Price price) {
        return side == Side::Buy ? level_for(bids, price) : level_for(asks, price);
    }

    template <class LevelMap>
    void erase_level_if_empty(LevelMap &levels, typename LevelMap::iterator level_it) {
        if (level_it->second.size == 0) {
            levels.erase(level_it);
        }
    }

    template <class LevelMap> void erase_from(LevelMap &levels, Price price, Node *node) {
        auto level_it = levels.find(price);
        unlink_node(level_it->second, node);
        destroy_node(node);
        erase_level_if_empty(levels, level_it);
    }

    void erase_resting_order(Locator loc) {
        if (loc.side == Side::Buy) {
            erase_from(bids, loc.price, loc.node);
            return;
        }
        erase_from(asks, loc.price, loc.node);
    }

    bool rest(OrderId id, Side side, Price price, Quantity quantity) {
        Order *order = orders.try_acquire(id, side, price, quantity);
        if (order == nullptr) {
            return false;
        }
        Node *node = nodes.try_acquire(Node{order, nullptr, nullptr});
        if (node == nullptr) {
            static_cast<void>(orders.release(order));
            return false;
        }
        Level &level = level_for(side, price);
        append_node(level, node);
        index[id] = Locator{side, price, node};
        return true;
    }

    static bool can_match_level(bool taker_is_buy, bool is_market, Price limit, Price level_price) {
        return OrderBook::can_match_level(taker_is_buy, is_market, limit, level_price);
    }

    void fill_front_order(Level &level, Price level_price, OrderBook::MatchContext &ctx) {
        Node *node = level.head;
        Order &maker = *node->order;
        if (OrderBook::fill_maker(ctx, maker.id, maker.quantity, level_price)) {
            index.erase(maker.id);
            unlink_node(level, node);
            destroy_node(node);
        }
    }

    void fill_level(Level &level, Price level_price, OrderBook::MatchContext &ctx) {
        while (ctx.quantity > 0 && level.head != nullptr) {
            fill_front_order(level, level_price, ctx);
        }
    }

    template <class OppMap> void match_against(OppMap &opposite, OrderBook::MatchContext &ctx) {
        while (ctx.quantity > 0 && !opposite.empty()) {
            auto level_it = opposite.begin();
            const Price level_price = level_it->first;
            if (!can_match_level(ctx.taker_is_buy, ctx.is_market, ctx.limit, level_price)) {
                break;
            }
            fill_level(level_it->second, level_price, ctx);
            erase_level_if_empty(opposite, level_it);
        }
    }

    void match_opposite(Side side, OrderBook::MatchContext &ctx) {
        if (side == Side::Buy) {
            match_against(asks, ctx);
            return;
        }
        match_against(bids, ctx);
    }

    struct MatchOutcome {
        Quantity remainder;
        std::size_t makers_freed;
    };

    static void simulate_node_match(const Node &node, MatchOutcome &outcome) {
        const Quantity traded = std::min(outcome.remainder, node.order->quantity);
        outcome.remainder -= traded;
        if (traded == node.order->quantity) {
            ++outcome.makers_freed;
        }
    }

    static void simulate_level_match(const Level &level, MatchOutcome &outcome) {
        for (const Node *node = level.head; outcome.remainder > 0 && node != nullptr;
             node = node->next) {
            simulate_node_match(*node, outcome);
        }
    }

    template <class OppMap>
    MatchOutcome simulate_match(const OppMap &opposite, bool taker_is_buy, Price limit,
                                bool is_market, Quantity quantity) const {
        MatchOutcome outcome{quantity, 0};
        for (auto level_it = opposite.begin(); outcome.remainder > 0 && level_it != opposite.end();
             ++level_it) {
            const Price level_price = level_it->first;
            if (!can_match_level(taker_is_buy, is_market, limit, level_price)) {
                break;
            }
            simulate_level_match(level_it->second, outcome);
        }
        return outcome;
    }

    [[nodiscard]] MatchOutcome simulate_match(Side side, Price limit, bool is_market,
                                              Quantity quantity) const {
        if (side == Side::Buy) {
            return simulate_match(asks, /*taker_is_buy=*/true, limit, is_market, quantity);
        }
        return simulate_match(bids, /*taker_is_buy=*/false, limit, is_market, quantity);
    }

    [[nodiscard]] bool can_store_limit(Side side, Price price, Quantity quantity,
                                       TimeInForce tif) const {
        if (tif == TimeInForce::IOC) {
            return true;
        }
        const MatchOutcome outcome = simulate_match(side, price, /*is_market=*/false, quantity);
        // A fully filled order rests nothing. Otherwise the remainder needs one order + node slot,
        // which any fully consumed maker frees during the match before the rest happens -- so a
        // match that frees at least one maker always fits, regardless of current free capacity.
        return outcome.remainder == 0 || outcome.makers_freed > 0 || has_capacity();
    }

    std::vector<Trade> add_limit(LimitInput input) {
        OrderBook::MatchResult result = OrderBook::match_incoming(
            input.id, input.side, input.price, /*is_market=*/false, input.quantity,
            [&] { return contains(input.id); },
            [&](Side side, OrderBook::MatchContext &ctx) { match_opposite(side, ctx); });
        // Rest only the post-match remainder: match_against reduced ctx.quantity as it filled, so a
        // fully filled order has ctx.quantity == 0 and must not rest, and a partial fill rests the
        // leftover, not the original input quantity.
        if (result.accepted && result.remainder > 0 && input.tif == TimeInForce::GTC) {
            static_cast<void>(rest(input.id, input.side, input.price, result.remainder));
        }
        return result.trades;
    }

    std::vector<Trade> add_market(OrderId id, Side side, Quantity quantity) {
        return OrderBook::match_incoming(
                   id, side, /*limit=*/0, /*is_market=*/true, quantity,
                   [&] { return contains(id); },
                   [&](Side taker_side, OrderBook::MatchContext &ctx) {
                       match_opposite(taker_side, ctx);
                   })
            .trades;
    }

    bool cancel(OrderId id) {
        const auto found = index.find(id);
        if (found == index.end()) {
            return false;
        }
        const Locator loc = found->second;
        erase_resting_order(loc);
        index.erase(found);
        return true;
    }

    std::vector<Trade> modify(OrderId id, Price new_price, Quantity new_quantity) {
        std::vector<Trade> trades;
        const auto found = index.find(id);
        if (found == index.end()) {
            return trades;
        }
        const Locator loc = found->second;
        if (new_quantity == 0) {
            static_cast<void>(cancel(id));
            return trades;
        }
        if (new_price == loc.price && new_quantity <= loc.node->order->quantity) {
            loc.node->order->quantity = new_quantity;
            return trades;
        }
        const Side side = loc.side;
        static_cast<void>(cancel(id));
        return add_limit(LimitInput{id, side, new_price, new_quantity, TimeInForce::GTC});
    }

    [[nodiscard]] std::optional<Price> best_bid() const {
        return bids.empty() ? std::nullopt : std::optional<Price>{bids.begin()->first};
    }

    [[nodiscard]] std::optional<Price> best_ask() const {
        return asks.empty() ? std::nullopt : std::optional<Price>{asks.begin()->first};
    }

    template <class LevelMap>
    [[nodiscard]] const Level *find_level(const LevelMap &levels, Price price) const {
        const auto it = levels.find(price);
        return it == levels.end() ? nullptr : &it->second;
    }

    [[nodiscard]] QuantityTotal quantity_at(Side side, Price price) const {
        const Level *level = side == Side::Buy ? find_level(bids, price) : find_level(asks, price);
        if (level == nullptr) {
            return 0;
        }
        QuantityTotal total = 0;
        for (Node *node = level->head; node != nullptr; node = node->next) {
            total += static_cast<QuantityTotal>(node->order->quantity);
        }
        return total;
    }

    [[nodiscard]] std::size_t order_count() const { return index.size(); }
    [[nodiscard]] bool contains(OrderId id) const { return index.find(id) != index.end(); }

    template <class OppMap>
    [[nodiscard]] std::size_t fill_count(const OppMap &opposite, bool taker_is_buy, Price limit,
                                         bool is_market, Quantity quantity) const {
        std::size_t count = 0;
        for (auto level_it = opposite.begin(); quantity > 0 && level_it != opposite.end();
             ++level_it) {
            const Price level_price = level_it->first;
            if (!can_match_level(taker_is_buy, is_market, limit, level_price)) {
                break;
            }
            for (Node *node = level_it->second.head; quantity > 0 && node != nullptr;
                 node = node->next) {
                quantity -= std::min(quantity, node->order->quantity);
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] std::size_t fill_count(Side taker_side, Price limit, bool is_market,
                                         Quantity quantity) const {
        if (taker_side == Side::Buy) {
            return fill_count(asks, /*taker_is_buy=*/true, limit, is_market, quantity);
        }
        if (taker_side == Side::Sell) {
            return fill_count(bids, /*taker_is_buy=*/false, limit, is_market, quantity);
        }
        return 0;
    }

    template <class LevelMap>
    [[nodiscard]] static std::vector<LevelView> collect_levels(const LevelMap &book) {
        std::vector<LevelView> levels;
        levels.reserve(book.size());
        for (const auto &[price, level] : book) {
            QuantityTotal total = 0;
            for (Node *node = level.head; node != nullptr; node = node->next) {
                total += static_cast<QuantityTotal>(node->order->quantity);
            }
            levels.push_back(LevelView{price, total});
        }
        return levels;
    }

    [[nodiscard]] std::vector<LevelView> bid_levels() const { return collect_levels(bids); }
    [[nodiscard]] std::vector<LevelView> ask_levels() const { return collect_levels(asks); }

    template <class LevelMap>
    static void append_orders(const LevelMap &book, std::vector<Order> &out) {
        for (const auto &entry : book) {
            for (Node *node = entry.second.head; node != nullptr; node = node->next) {
                out.push_back(*node->order);
            }
        }
    }

    [[nodiscard]] std::vector<Order> resting_orders() const {
        std::vector<Order> out;
        out.reserve(index.size());
        append_orders(bids, out);
        append_orders(asks, out);
        return out;
    }
};

// M47 contiguous storage: price levels live in a flat array indexed directly by price tick
// (band [kContiguousMinPrice, kContiguousMaxPrice]); per-side occupancy bitmaps answer
// best-price queries; each level's FIFO queue is a contiguous vector with a head cursor and
// tombstoned cancels. The band constrains *resting* only — out-of-band limit prices still
// match — and an order whose remainder would rest out of band is refused via can_store_limit.
struct OrderBook::ContiguousStore {
    static constexpr Price kMinPrice = OrderBook::kContiguousMinPrice;
    static constexpr Price kMaxPrice = OrderBook::kContiguousMaxPrice;
    static constexpr std::size_t kBandWidth = static_cast<std::size_t>(kMaxPrice - kMinPrice + 1);
    static constexpr std::size_t kBitmapWords = (kBandWidth + 63) / 64;
    // A level compacts when tombstones outnumber live orders past this size, so churn at one
    // price cannot grow its queue without bound.
    static constexpr std::size_t kCompactMinSize = 16;

    struct FlatOrder {
        OrderId id;
        Quantity quantity; // 0 marks a tombstone; live orders always have quantity > 0
    };

    struct FlatLevel {
        std::vector<FlatOrder> orders;
        std::uint32_t head = 0; // index of the first live order while alive > 0
        std::uint32_t alive = 0;
    };

    struct Locator {
        Side side;
        Price price;
        std::uint32_t pos;
    };

    using Bitmap = std::array<std::uint64_t, kBitmapWords>;

    struct BookSide {
        std::vector<FlatLevel> levels{kBandWidth};
        Bitmap bits{};
    };

    BookSide bids;
    BookSide asks;
    std::unordered_map<OrderId, Locator> index;

    static bool in_band(Price price) noexcept { return price >= kMinPrice && price <= kMaxPrice; }
    static std::size_t slot_of(Price price) noexcept {
        return static_cast<std::size_t>(price - kMinPrice);
    }
    static Price price_of(std::size_t slot) noexcept {
        return kMinPrice + static_cast<Price>(slot);
    }

    BookSide &side_ref(Side side) { return side == Side::Buy ? bids : asks; }
    const BookSide &side_ref(Side side) const { return side == Side::Buy ? bids : asks; }

    static void set_bit(Bitmap &bits, std::size_t slot) noexcept {
        bits[slot / 64] |= std::uint64_t{1} << (slot % 64);
    }
    static void clear_bit(Bitmap &bits, std::size_t slot) noexcept {
        bits[slot / 64] &= ~(std::uint64_t{1} << (slot % 64));
    }

    // Lowest occupied slot at or above `from` (ask side best-first order), or nullopt.
    static std::optional<std::size_t> next_up(const Bitmap &bits, std::size_t from) noexcept {
        for (std::size_t word = from / 64; word < kBitmapWords; ++word) {
            std::uint64_t value = bits[word];
            if (word == from / 64) {
                value &= ~std::uint64_t{0} << (from % 64);
            }
            if (value != 0) {
                return word * 64 + static_cast<std::size_t>(std::countr_zero(value));
            }
        }
        return std::nullopt;
    }

    // Highest occupied slot at or below `from` (bid side best-first order), or nullopt.
    static std::optional<std::size_t> next_down(const Bitmap &bits, std::size_t from) noexcept {
        for (std::size_t word = from / 64 + 1; word-- > 0;) {
            std::uint64_t value = bits[word];
            if (word == from / 64 && (from % 64) != 63) {
                value &= (std::uint64_t{1} << ((from % 64) + 1)) - 1;
            }
            if (value != 0) {
                return word * 64 + (63 - static_cast<std::size_t>(std::countl_zero(value)));
            }
        }
        return std::nullopt;
    }

    static std::optional<std::size_t> best_slot(const BookSide &side, bool is_bid) noexcept {
        return is_bid ? next_down(side.bits, kBandWidth - 1) : next_up(side.bits, 0);
    }

    // The next occupied slot strictly worse than `slot` for the given side, or nullopt.
    static std::optional<std::size_t> next_worse(const Bitmap &bits, bool is_bid,
                                                 std::size_t slot) noexcept {
        if (is_bid) {
            return slot == 0 ? std::optional<std::size_t>{} : next_down(bits, slot - 1);
        }
        return slot + 1 >= kBandWidth ? std::optional<std::size_t>{} : next_up(bits, slot + 1);
    }

    // Visit occupied levels best-first; `fn(slot, level)` returns false to stop early.
    template <class Fn> void for_each_occupied(const BookSide &side, bool is_bid, Fn &&fn) const {
        auto slot = best_slot(side, is_bid);
        while (slot) {
            if (!fn(*slot, side.levels[*slot])) {
                return;
            }
            slot = next_worse(side.bits, is_bid, *slot);
        }
    }

    static void advance_head(FlatLevel &level) noexcept {
        while (level.head < level.orders.size() && level.orders[level.head].quantity == 0) {
            ++level.head;
        }
    }

    void clear_level(BookSide &side, std::size_t slot) {
        FlatLevel &level = side.levels[slot];
        level.orders.clear();
        level.head = 0;
        level.alive = 0;
        clear_bit(side.bits, slot);
    }

    // Rewrite the FIFO with live orders only (preserving time priority) and refresh their
    // locator positions.
    void compact_level(FlatLevel &level) {
        std::vector<FlatOrder> live;
        live.reserve(level.alive);
        for (std::size_t i = level.head; i < level.orders.size(); ++i) {
            if (level.orders[i].quantity > 0) {
                auto found = index.find(level.orders[i].id);
                if (found != index.end()) {
                    found->second.pos = static_cast<std::uint32_t>(live.size());
                }
                live.push_back(level.orders[i]);
            }
        }
        level.orders = std::move(live);
        level.head = 0;
    }

    void maybe_compact(FlatLevel &level) {
        const std::size_t dead = level.orders.size() - level.alive;
        if (level.orders.size() >= kCompactMinSize && dead > level.alive) {
            compact_level(level);
        }
    }

    bool rest(OrderId id, Side side, Price price, Quantity quantity) {
        if (!in_band(price)) {
            return false;
        }
        BookSide &book_side = side_ref(side);
        const std::size_t slot = slot_of(price);
        FlatLevel &level = book_side.levels[slot];
        if (level.alive == 0) {
            set_bit(book_side.bits, slot);
        }
        index[id] = Locator{side, price, static_cast<std::uint32_t>(level.orders.size())};
        level.orders.push_back(FlatOrder{id, quantity});
        ++level.alive;
        return true;
    }

    void fill_front_order(FlatLevel &level, Price level_price, OrderBook::MatchContext &ctx) {
        FlatOrder &maker = level.orders[level.head];
        if (OrderBook::fill_maker(ctx, maker.id, maker.quantity, level_price)) {
            index.erase(maker.id);
            --level.alive;
            advance_head(level);
        }
    }

    void fill_level(FlatLevel &level, Price level_price, OrderBook::MatchContext &ctx) {
        while (ctx.quantity > 0 && level.alive > 0) {
            fill_front_order(level, level_price, ctx);
        }
    }

    void match_against(BookSide &opposite, bool opposite_is_bid, OrderBook::MatchContext &ctx) {
        while (ctx.quantity > 0) {
            const auto slot = best_slot(opposite, opposite_is_bid);
            if (!slot) {
                return;
            }
            const Price level_price = price_of(*slot);
            if (!OrderBook::can_match_level(ctx.taker_is_buy, ctx.is_market, ctx.limit,
                                            level_price)) {
                return;
            }
            FlatLevel &level = opposite.levels[*slot];
            fill_level(level, level_price, ctx);
            if (level.alive == 0) {
                clear_level(opposite, *slot);
            } else {
                maybe_compact(level); // fills leave a dead prefix; keep long-lived levels bounded
            }
        }
    }

    void match_opposite(Side side, OrderBook::MatchContext &ctx) {
        const bool taker_is_buy = side == Side::Buy;
        match_against(taker_is_buy ? asks : bids, !taker_is_buy, ctx);
    }

    std::vector<Trade> add_limit(OrderId id, Side side, Price price, Quantity quantity,
                                 TimeInForce tif) {
        OrderBook::MatchResult result = OrderBook::match_incoming(
            id, side, price, /*is_market=*/false, quantity, [&] { return contains(id); },
            [&](Side taker_side, OrderBook::MatchContext &ctx) {
                match_opposite(taker_side, ctx);
            });
        if (result.accepted && result.remainder > 0 && tif == TimeInForce::GTC) {
            static_cast<void>(rest(id, side, price, result.remainder));
        }
        return result.trades;
    }

    std::vector<Trade> add_market(OrderId id, Side side, Quantity quantity) {
        return OrderBook::match_incoming(
                   id, side, /*limit=*/0, /*is_market=*/true, quantity,
                   [&] { return contains(id); },
                   [&](Side taker_side, OrderBook::MatchContext &ctx) {
                       match_opposite(taker_side, ctx);
                   })
            .trades;
    }

    void remove_resting(const Locator &loc) {
        BookSide &book_side = side_ref(loc.side);
        const std::size_t slot = slot_of(loc.price);
        FlatLevel &level = book_side.levels[slot];
        level.orders[loc.pos].quantity = 0;
        --level.alive;
        if (level.alive == 0) {
            clear_level(book_side, slot);
            return;
        }
        if (loc.pos == level.head) {
            advance_head(level);
        }
        maybe_compact(level);
    }

    bool cancel(OrderId id) {
        const auto found = index.find(id);
        if (found == index.end()) {
            return false;
        }
        const Locator loc = found->second;
        index.erase(found);
        remove_resting(loc);
        return true;
    }

    std::vector<Trade> modify(OrderId id, Price new_price, Quantity new_quantity) {
        std::vector<Trade> trades;
        const auto found = index.find(id);
        if (found == index.end()) {
            return trades;
        }
        const Locator loc = found->second;
        if (new_quantity == 0) {
            static_cast<void>(cancel(id));
            return trades;
        }
        FlatOrder &order = side_ref(loc.side).levels[slot_of(loc.price)].orders[loc.pos];
        if (new_price == loc.price && new_quantity <= order.quantity) {
            order.quantity = new_quantity;
            return trades;
        }
        // The re-add may cross. The band constrains resting: refuse a reprice whose remainder
        // would rest out of band, keeping the original order intact instead of dropping it.
        if (!in_band(new_price) && simulate_remainder(loc.side, new_price, new_quantity) > 0) {
            return trades;
        }
        static_cast<void>(cancel(id));
        return add_limit(id, loc.side, new_price, new_quantity, TimeInForce::GTC);
    }

    // The remainder a limit order would have left after crossing the current opposite side.
    [[nodiscard]] Quantity simulate_remainder(Side side, Price limit, Quantity quantity) const {
        const bool taker_is_buy = side == Side::Buy;
        const BookSide &opposite = taker_is_buy ? asks : bids;
        for_each_occupied(opposite, !taker_is_buy, [&](std::size_t slot, const FlatLevel &level) {
            if (quantity == 0 || !OrderBook::can_match_level(taker_is_buy, /*is_market=*/false,
                                                             limit, price_of(slot))) {
                return false;
            }
            for (std::size_t i = level.head; quantity > 0 && i < level.orders.size(); ++i) {
                quantity -= std::min(quantity, level.orders[i].quantity); // tombstones add 0
            }
            return quantity > 0;
        });
        return quantity;
    }

    static std::size_t count_flat_level(const FlatLevel &level, Quantity &quantity) {
        std::size_t count = 0;
        for (std::size_t i = level.head; quantity > 0 && i < level.orders.size(); ++i) {
            if (level.orders[i].quantity > 0) {
                quantity -= std::min(quantity, level.orders[i].quantity);
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] std::size_t count_flat_matches(const BookSide &opposite, bool opposite_is_bid,
                                                 OrderBook::MatchQuery query) const {
        std::size_t count = 0;
        for_each_occupied(opposite, opposite_is_bid, [&](std::size_t slot, const FlatLevel &level) {
            if (query.quantity == 0 ||
                !OrderBook::can_match_level(query.taker_is_buy, query.is_market, query.limit,
                                            price_of(slot))) {
                return false;
            }
            count += count_flat_level(level, query.quantity);
            return query.quantity > 0;
        });
        return count;
    }

    [[nodiscard]] bool can_store_limit(Side side, Price price, Quantity quantity,
                                       TimeInForce tif) const {
        if (tif == TimeInForce::IOC || in_band(price)) {
            return true;
        }
        return simulate_remainder(side, price, quantity) == 0;
    }

    [[nodiscard]] std::optional<Price> best_bid() const {
        const auto slot = best_slot(bids, /*is_bid=*/true);
        return slot ? std::optional<Price>{price_of(*slot)} : std::nullopt;
    }

    [[nodiscard]] std::optional<Price> best_ask() const {
        const auto slot = best_slot(asks, /*is_bid=*/false);
        return slot ? std::optional<Price>{price_of(*slot)} : std::nullopt;
    }

    static QuantityTotal level_total(const FlatLevel &level) {
        QuantityTotal total = 0;
        for (std::size_t i = level.head; i < level.orders.size(); ++i) {
            total += static_cast<QuantityTotal>(level.orders[i].quantity);
        }
        return total;
    }

    [[nodiscard]] QuantityTotal quantity_at(Side side, Price price) const {
        if (!in_band(price)) {
            return 0;
        }
        return level_total(side_ref(side).levels[slot_of(price)]);
    }

    [[nodiscard]] std::size_t order_count() const { return index.size(); }
    [[nodiscard]] bool contains(OrderId id) const { return index.find(id) != index.end(); }

    [[nodiscard]] std::size_t fill_count(Side taker_side, Price limit, bool is_market,
                                         Quantity quantity) const {
        if (taker_side != Side::Buy && taker_side != Side::Sell) {
            return 0;
        }
        const bool taker_is_buy = taker_side == Side::Buy;
        const BookSide &opposite = taker_is_buy ? asks : bids;
        return count_flat_matches(opposite, !taker_is_buy,
                                  OrderBook::MatchQuery{taker_is_buy, limit, is_market, quantity});
    }

    [[nodiscard]] std::vector<LevelView> collect_levels(const BookSide &side, bool is_bid) const {
        std::vector<LevelView> levels;
        for_each_occupied(side, is_bid, [&](std::size_t slot, const FlatLevel &level) {
            levels.push_back(LevelView{price_of(slot), level_total(level)});
            return true;
        });
        return levels;
    }

    [[nodiscard]] std::vector<LevelView> bid_levels() const {
        return collect_levels(bids, /*is_bid=*/true);
    }
    [[nodiscard]] std::vector<LevelView> ask_levels() const {
        return collect_levels(asks, /*is_bid=*/false);
    }

    void append_orders(const BookSide &side, bool is_bid, Side side_value,
                       std::vector<Order> &out) const {
        for_each_occupied(side, is_bid, [&](std::size_t slot, const FlatLevel &level) {
            for (std::size_t i = level.head; i < level.orders.size(); ++i) {
                if (level.orders[i].quantity > 0) {
                    out.push_back(Order{level.orders[i].id, side_value, price_of(slot),
                                        level.orders[i].quantity});
                }
            }
            return true;
        });
    }

    [[nodiscard]] std::vector<Order> resting_orders() const {
        std::vector<Order> out;
        out.reserve(index.size());
        append_orders(bids, /*is_bid=*/true, Side::Buy, out);
        append_orders(asks, /*is_bid=*/false, Side::Sell, out);
        return out;
    }
};

// Baseline uses the shared new_delete_resource (operator new/delete -> identical to pre-M32).
// Pooled owns an unsynchronized_pool_resource; resource_ is set before the pmr containers (which
// hold polymorphic_allocators referencing it) are constructed. Per-price FIFO lists are explicitly
// constructed with resource_ when a new level is inserted.
OrderBook::OrderBook(Storage storage)
    : pool_(storage == Storage::Pooled
                ? std::optional<std::pmr::unsynchronized_pool_resource>(std::in_place)
                : std::nullopt),
      resource_(pool_.has_value() ? &pool_.value() : std::pmr::new_delete_resource()),
      bids_(resource_), asks_(resource_), index_(resource_),
      intrusive_(storage == Storage::IntrusivePooled ? std::make_unique<IntrusiveStore>()
                                                     : nullptr),
      contiguous_(storage == Storage::Contiguous ? std::make_unique<ContiguousStore>() : nullptr) {}

OrderBook::~OrderBook() = default;

bool OrderBook::can_match_level(bool taker_is_buy, bool is_market, Price limit, Price level_price) {
    if (is_market) {
        return true;
    }
    return taker_is_buy ? level_price <= limit : level_price >= limit;
}

void OrderBook::fill_front_order(Level &level, Price level_price, MatchContext &ctx) {
    Order &maker = level.front();
    if (fill_maker(ctx, maker.id, maker.quantity, level_price)) {
        index_.erase(maker.id);
        level.pop_front();
    }
}

void OrderBook::fill_level(Level &level, Price level_price, MatchContext &ctx) {
    while (ctx.quantity > 0 && !level.empty()) {
        fill_front_order(level, level_price, ctx);
    }
}

template <class LevelMap>
void OrderBook::erase_level_if_empty(LevelMap &levels, typename LevelMap::iterator level_it) {
    if (level_it->second.empty()) {
        levels.erase(level_it);
    }
}

template <class OppMap> void OrderBook::match_against(OppMap &opposite, MatchContext &ctx) {
    while (ctx.quantity > 0 && !opposite.empty()) {
        auto level_it = opposite.begin(); // best price on the opposite side
        const Price level_price = level_it->first;
        if (!can_match_level(ctx.taker_is_buy, ctx.is_market, ctx.limit, level_price)) {
            break;
        }
        fill_level(level_it->second, level_price, ctx);
        erase_level_if_empty(opposite, level_it);
    }
}

void OrderBook::match_baseline(Side side, MatchContext &ctx) {
    if (side == Side::Buy) {
        match_against(asks_, ctx);
        return;
    }
    match_against(bids_, ctx);
}

template <class Level> std::size_t count_level_matches(const Level &level, Quantity &quantity) {
    std::size_t count = 0;
    for (auto order_it = level.begin(); quantity > 0 && order_it != level.end(); ++order_it) {
        const Order &maker = *order_it;
        const Quantity traded = std::min(quantity, maker.quantity);
        quantity -= traded;
        ++count;
    }
    return count;
}

template <class OppMap>
std::size_t OrderBook::count_matches(const OppMap &opposite, MatchQuery query) const {
    std::size_t count = 0;
    for (auto level_it = opposite.begin(); query.quantity > 0 && level_it != opposite.end();
         ++level_it) {
        const Price level_price = level_it->first;
        if (!can_match_level(query.taker_is_buy, query.is_market, query.limit, level_price)) {
            break;
        }
        count += count_level_matches(level_it->second, query.quantity);
    }
    return count;
}

OrderBook::Level &OrderBook::level_for(Side side, Price price) {
    if (side == Side::Buy) {
        auto [it, inserted] = bids_.emplace(price, Level{Level::allocator_type{resource_}});
        (void)inserted;
        return it->second;
    }
    auto [it, inserted] = asks_.emplace(price, Level{Level::allocator_type{resource_}});
    (void)inserted;
    return it->second;
}

void OrderBook::rest(OrderId id, Side side, Price price, Quantity quantity) {
    Level &level = level_for(side, price);
    level.push_back(Order{id, side, price, quantity});
    index_[id] = Locator{side, price, std::prev(level.end())};
}

template <class BaselineFn, class IntrusiveFn, class ContiguousFn>
decltype(auto) OrderBook::dispatch_storage(BaselineFn &&baseline_fn, IntrusiveFn &&intrusive_fn,
                                           ContiguousFn &&contiguous_fn) {
    if (intrusive_) {
        return intrusive_fn(*intrusive_);
    }
    if (contiguous_) {
        return contiguous_fn(*contiguous_);
    }
    return baseline_fn();
}

template <class BaselineFn, class IntrusiveFn, class ContiguousFn>
decltype(auto) OrderBook::dispatch_storage(BaselineFn &&baseline_fn, IntrusiveFn &&intrusive_fn,
                                           ContiguousFn &&contiguous_fn) const {
    if (intrusive_) {
        return intrusive_fn(*intrusive_);
    }
    if (contiguous_) {
        return contiguous_fn(*contiguous_);
    }
    return baseline_fn();
}

namespace {
template <class LevelMap> std::optional<Price> best_price(const LevelMap &levels) {
    if (levels.empty()) {
        return std::nullopt;
    }
    return levels.begin()->first;
}
} // namespace

std::vector<Trade> OrderBook::add_limit(OrderId id, Side side, Price price, Quantity quantity,
                                        TimeInForce tif) {
    return dispatch_storage(
        [&] {
            MatchResult result = match_incoming(
                id, side, price, /*is_market=*/false, quantity, [&] { return contains(id); },
                [&](Side taker_side, MatchContext &ctx) { match_baseline(taker_side, ctx); });
            if (result.accepted && result.remainder > 0 && tif == TimeInForce::GTC) {
                rest(id, side, price, result.remainder);
            }
            return result.trades;
        },
        [&](IntrusiveStore &store) {
            return store.add_limit(IntrusiveStore::LimitInput{id, side, price, quantity, tif});
        },
        [&](ContiguousStore &store) { return store.add_limit(id, side, price, quantity, tif); });
}

std::vector<Trade> OrderBook::add_market(OrderId id, Side side, Quantity quantity) {
    return dispatch_storage(
        [&] {
            return match_incoming(
                       id, side, /*limit=*/0, /*is_market=*/true, quantity,
                       [&] { return contains(id); },
                       [&](Side taker_side, MatchContext &ctx) { match_baseline(taker_side, ctx); })
                .trades;
        },
        [&](IntrusiveStore &store) { return store.add_market(id, side, quantity); },
        [&](ContiguousStore &store) { return store.add_market(id, side, quantity); });
}

template <class LevelMap> void OrderBook::erase_from_side(LevelMap &levels, const Locator &loc) {
    auto level_it = levels.find(loc.price);
    level_it->second.erase(loc.it);
    erase_level_if_empty(levels, level_it);
}

void OrderBook::erase_resting_order(const Locator &loc) {
    if (loc.side == Side::Buy) {
        erase_from_side(bids_, loc);
        return;
    }
    erase_from_side(asks_, loc);
}

bool OrderBook::cancel(OrderId id) {
    return dispatch_storage(
        [&] {
            const auto found = index_.find(id);
            if (found == index_.end()) {
                return false;
            }
            const Locator loc = found->second;
            erase_resting_order(loc);
            index_.erase(found);
            return true;
        },
        [&](IntrusiveStore &store) { return store.cancel(id); },
        [&](ContiguousStore &store) { return store.cancel(id); });
}

std::vector<Trade> OrderBook::modify(OrderId id, Price new_price, Quantity new_quantity) {
    return dispatch_storage(
        [&] {
            std::vector<Trade> trades;
            const auto found = index_.find(id);
            if (found == index_.end()) {
                return trades; // unknown order: no-op here; rejection is the risk layer's job (M5)
            }
            const Locator loc = found->second;
            if (new_quantity == 0) {
                cancel(id);
                return trades;
            }
            if (new_price == loc.price && new_quantity <= loc.it->quantity) {
                loc.it->quantity = new_quantity; // reduce in place, preserve time priority
                return trades;
            }
            // Price change or quantity increase loses priority: re-add at the tail (may cross).
            const Side side = loc.side;
            cancel(id);
            return add_limit(id, side, new_price, new_quantity, TimeInForce::GTC);
        },
        [&](IntrusiveStore &store) { return store.modify(id, new_price, new_quantity); },
        [&](ContiguousStore &store) { return store.modify(id, new_price, new_quantity); });
}

std::optional<Price> OrderBook::best_bid() const {
    return dispatch_storage([&] { return best_price(bids_); },
                            [](const IntrusiveStore &store) { return store.best_bid(); },
                            [](const ContiguousStore &store) { return store.best_bid(); });
}

std::optional<Price> OrderBook::best_ask() const {
    return dispatch_storage([&] { return best_price(asks_); },
                            [](const IntrusiveStore &store) { return store.best_ask(); },
                            [](const ContiguousStore &store) { return store.best_ask(); });
}

QuantityTotal OrderBook::quantity_at(Side side, Price price) const {
    return dispatch_storage(
        [&] {
            const Level *level = find_level(side, price);
            return level == nullptr ? QuantityTotal{0} : level_quantity(*level);
        },
        [&](const IntrusiveStore &store) { return store.quantity_at(side, price); },
        [&](const ContiguousStore &store) { return store.quantity_at(side, price); });
}

const OrderBook::Level *OrderBook::find_level(Side side, Price price) const {
    if (side == Side::Buy) {
        const auto it = bids_.find(price);
        return it == bids_.end() ? nullptr : &it->second;
    }
    const auto it = asks_.find(price);
    return it == asks_.end() ? nullptr : &it->second;
}

QuantityTotal OrderBook::level_quantity(const Level &level) {
    QuantityTotal total = 0;
    for (const Order &o : level) {
        total += static_cast<QuantityTotal>(o.quantity);
    }
    return total;
}

std::size_t OrderBook::order_count() const {
    return dispatch_storage([&] { return index_.size(); },
                            [](const IntrusiveStore &store) { return store.order_count(); },
                            [](const ContiguousStore &store) { return store.order_count(); });
}

bool OrderBook::contains(OrderId id) const {
    return dispatch_storage([&] { return index_.find(id) != index_.end(); },
                            [&](const IntrusiveStore &store) { return store.contains(id); },
                            [&](const ContiguousStore &store) { return store.contains(id); });
}

std::size_t OrderBook::fill_count(Side taker_side, Price limit, bool is_market,
                                  Quantity quantity) const {
    return dispatch_storage(
        [&] {
            if (taker_side == Side::Buy) {
                return count_matches(asks_,
                                     MatchQuery{/*taker_is_buy=*/true, limit, is_market, quantity});
            }
            if (taker_side == Side::Sell) {
                return count_matches(
                    bids_, MatchQuery{/*taker_is_buy=*/false, limit, is_market, quantity});
            }
            return std::size_t{0};
        },
        [&](const IntrusiveStore &store) {
            return store.fill_count(taker_side, limit, is_market, quantity);
        },
        [&](const ContiguousStore &store) {
            return store.fill_count(taker_side, limit, is_market, quantity);
        });
}

bool OrderBook::can_store_limit(Side side, Price price, Quantity quantity, TimeInForce tif) const {
    return dispatch_storage([] { return true; },
                            [&](const IntrusiveStore &store) {
                                return store.can_store_limit(side, price, quantity, tif);
                            },
                            [&](const ContiguousStore &store) {
                                return store.can_store_limit(side, price, quantity, tif);
                            });
}

namespace {
template <class LevelMap> std::vector<LevelView> collect_levels(const LevelMap &book) {
    std::vector<LevelView> levels;
    levels.reserve(book.size());
    for (const auto &[price, orders] : book) {
        QuantityTotal total = 0;
        for (const Order &o : orders) {
            total += static_cast<QuantityTotal>(o.quantity);
        }
        levels.push_back(LevelView{price, total});
    }
    return levels;
}
} // namespace

std::vector<LevelView> OrderBook::bid_levels() const {
    return dispatch_storage([&] { return collect_levels(bids_); },
                            [](const IntrusiveStore &store) { return store.bid_levels(); },
                            [](const ContiguousStore &store) { return store.bid_levels(); });
}

std::vector<LevelView> OrderBook::ask_levels() const {
    return dispatch_storage([&] { return collect_levels(asks_); },
                            [](const IntrusiveStore &store) { return store.ask_levels(); },
                            [](const ContiguousStore &store) { return store.ask_levels(); });
}

namespace {
template <class LevelMap> void append_orders(const LevelMap &book, std::vector<Order> &out) {
    for (const auto &entry : book) {
        for (const Order &o : entry.second) {
            out.push_back(o);
        }
    }
}
} // namespace

std::vector<Order> OrderBook::resting_orders() const {
    return dispatch_storage(
        [&] {
            std::vector<Order> out;
            out.reserve(index_.size());
            append_orders(bids_, out);
            append_orders(asks_, out);
            return out;
        },
        [](const IntrusiveStore &store) { return store.resting_orders(); },
        [](const ContiguousStore &store) { return store.resting_orders(); });
}

} // namespace qsl::engine
