#include "qsl/engine/order_book.hpp"

#include "contiguous_store.hpp"
#include "qsl/memory/order_pool.hpp"

#include <algorithm>
#include <array>
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

template <class OppMap, class FillLevelFn, class EraseEmptyFn>
void OrderBook::match_ordered_levels(OppMap &opposite, MatchContext &ctx,
                                     FillLevelFn &&fill_level_fn, EraseEmptyFn &&erase_empty_fn) {
    while (ctx.quantity > 0 && !opposite.empty()) {
        auto level_it = opposite.begin();
        const Price level_price = level_it->first;
        if (!can_match_level(ctx.taker_is_buy, ctx.is_market, ctx.limit, level_price)) {
            break;
        }
        fill_level_fn(level_it->second, level_price);
        erase_empty_fn(level_it);
    }
}

template <class Index, class RemoveFn>
bool OrderBook::cancel_indexed_order(Index &index, OrderId id, RemoveFn &&remove_fn) {
    const auto found = index.find(id);
    if (found == index.end()) {
        return false;
    }
    const auto loc = found->second;
    index.erase(found);
    std::forward<RemoveFn>(remove_fn)(loc);
    return true;
}

bool OrderBook::fill_maker(MatchContext &ctx, OrderId maker_id, Quantity &maker_quantity,
                           Price level_price) {
    const Quantity traded = std::min(ctx.quantity, maker_quantity);
    ctx.trades.push_back(Trade{ctx.taker_id, maker_id, level_price, traded});
    ctx.quantity -= traded;
    maker_quantity -= traded;
    return maker_quantity == 0;
}

bool OrderBook::should_rest_limit(const MatchResult &result, TimeInForce tif) noexcept {
    if (!result.accepted) {
        return false;
    }
    if (result.remainder == 0) {
        return false;
    }
    return tif == TimeInForce::GTC;
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

    struct RestingNode {
        OrderId id;
        Side side;
        Price price;
        Node *node;
    };

    using BidMap = std::map<Price, Level, std::greater<Price>>;
    using AskMap = std::map<Price, Level, std::less<Price>>;
    using Index = std::unordered_map<OrderId, Locator>;

    memory::OrderPool<kIntrusivePoolCapacity> orders;
    RawPool<Node, kIntrusivePoolCapacity> nodes;
    BidMap bids;
    AskMap asks;
    Index index;

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

    void erase_indexed_order(Index::iterator found) {
        const Locator loc = found->second;
        index.erase(found);
        erase_resting_order(loc);
    }

    template <class LevelMap> bool append_resting(LevelMap &levels, RestingNode resting) {
        auto level_it = levels.try_emplace(resting.price).first;
        Level &level = level_it->second;
        append_node(level, resting.node);
        const auto [it, inserted] =
            index.emplace(resting.id, Locator{resting.side, resting.price, resting.node});
        (void)it;
        if (!inserted) {
            unlink_node(level, resting.node);
            destroy_node(resting.node);
            erase_level_if_empty(levels, level_it);
            return false;
        }
        return true;
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
        if (side == Side::Buy) {
            return append_resting(bids, RestingNode{id, side, price, node});
        }
        return append_resting(asks, RestingNode{id, side, price, node});
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
        OrderBook::match_ordered_levels(
            opposite, ctx,
            [&](Level &level, Price level_price) { fill_level(level, level_price, ctx); },
            [&](typename OppMap::iterator level_it) { erase_level_if_empty(opposite, level_it); });
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
        if (has_capacity()) {
            return true;
        }
        const MatchOutcome outcome = simulate_match(side, price, /*is_market=*/false, quantity);
        // A fully filled order rests nothing. Otherwise the remainder needs one order + node slot,
        // which any fully consumed maker frees during the match before the rest happens -- so a
        // match that frees at least one maker fits even when the pool is currently full.
        return outcome.remainder == 0 || outcome.makers_freed > 0;
    }

    std::vector<Trade> add_limit(LimitInput input) {
        if (!can_store_limit(input.side, input.price, input.quantity, input.tif)) {
            return {};
        }
        OrderBook::MatchResult result = OrderBook::match_incoming(
            input.id, input.side, input.price, /*is_market=*/false, input.quantity,
            [&] { return contains(input.id); },
            [&](Side side, OrderBook::MatchContext &ctx) { match_opposite(side, ctx); });
        // Rest only the post-match remainder: match_against reduced ctx.quantity as it filled, so a
        // fully filled order has ctx.quantity == 0 and must not rest, and a partial fill rests the
        // leftover, not the original input quantity.
        if (OrderBook::should_rest_limit(result, input.tif)) {
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
        erase_indexed_order(found);
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
            erase_indexed_order(found);
            return trades;
        }
        if (new_price == loc.price && new_quantity <= loc.node->order->quantity) {
            loc.node->order->quantity = new_quantity;
            return trades;
        }
        const Side side = loc.side;
        erase_indexed_order(found);
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
      contiguous_(storage == Storage::Contiguous ? std::make_unique<ContiguousStore>() : nullptr) {
    // The order index is on the hot path: every new/cancel/modify/fill does 1-4 point lookups on
    // it. Capping the load factor at 0.25 (vs the default 1.0) keeps probe chains short, which
    // measurably speeds the whole engine on a busy book — a measured ~+18% on the steady-state
    // profile workload, trading a modest amount of memory (more empty buckets) for fewer
    // collisions. This only changes bucket count, never iteration-for-output: index_ is used solely
    // for find/insert/erase/size, while snapshots and resting_orders() iterate the ordered
    // bids_/asks_ maps, so determinism is unaffected.
    index_.max_load_factor(0.25F);
}

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
    match_ordered_levels(
        opposite, ctx,
        [&](Level &level, Price level_price) { fill_level(level, level_price, ctx); },
        [&](typename OppMap::iterator level_it) { erase_level_if_empty(opposite, level_it); });
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
    // try_emplace avoids allocating+freeing a map node (std::map::emplace constructs the node
    // before checking for a duplicate key) and constructing a throwaway pmr list when the price
    // level already exists — the steady-state common case with a bounded price band. The intrusive
    // store already uses this; this brings the baseline path in line. Semantics are identical: an
    // absent level is inserted empty with the same pmr allocator, and erase_level_if_empty still
    // prunes it. The map carries resource_, so pmr scoped-allocator propagation constructs the
    // inserted Level with that same resource — no explicit allocator argument needed (matching the
    // intrusive store).
    if (side == Side::Buy) {
        return bids_.try_emplace(price).first->second;
    }
    return asks_.try_emplace(price).first->second;
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
            if (should_rest_limit(result, tif)) {
                rest(id, side, price, result.remainder);
            }
            return result.trades;
        },
        [&](IntrusiveStore &store) {
            return store.add_limit(LimitInput{id, side, price, quantity, tif});
        },
        [&](ContiguousStore &store) {
            return store.add_limit(LimitInput{id, side, price, quantity, tif});
        });
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
            return cancel_indexed_order(index_, id,
                                        [&](const Locator &loc) { erase_resting_order(loc); });
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

bool OrderBook::can_apply_modify(OrderId id, Price new_price, Quantity new_quantity) const {
    // Baseline/pooled modifies always apply, and an intrusive cancel+re-add always fits because
    // the cancel frees its own pool slots first; only the contiguous band can refuse.
    return dispatch_storage([] { return true; }, [](const IntrusiveStore &) { return true; },
                            [&](const ContiguousStore &store) {
                                return store.can_apply_modify(id, new_price, new_quantity);
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
