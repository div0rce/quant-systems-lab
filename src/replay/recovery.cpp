#include "qsl/replay/recovery.hpp"

#include <algorithm>
#include <random>
#include <string>
#include <variant>

namespace qsl::replay {

std::vector<EngineEvent> apply(MatchingEngine &engine, const Command &command) {
    if (const auto *c = std::get_if<RegisterSymbol>(&command)) {
        engine.register_symbol(c->name);
        return {};
    }
    if (const auto *c = std::get_if<NewLimit>(&command)) {
        return engine.new_limit(c->symbol, c->id, c->side, c->price, c->quantity, c->tif);
    }
    if (const auto *c = std::get_if<NewMarket>(&command)) {
        return engine.new_market(c->symbol, c->id, c->side, c->quantity);
    }
    if (const auto *c = std::get_if<Cancel>(&command)) {
        return engine.cancel(c->symbol, c->id);
    }
    const auto &c = std::get<Modify>(command);
    return engine.modify(c.symbol, c.id, c.price, c.quantity);
}

std::vector<EngineEvent> replay(MatchingEngine &engine, const std::vector<LogRecord> &records) {
    std::vector<EngineEvent> events;
    for (const LogRecord &record : records) {
        if (record.type != RecordType::CommandRecord) {
            continue;
        }
        const auto command = decode_command(record.payload);
        if (!command) {
            continue;
        }
        const auto produced = apply(engine, *command);
        events.insert(events.end(), produced.begin(), produced.end());
    }
    return events;
}

namespace {

struct ActiveOrder {
    SymbolId symbol;
    OrderId id;
    Side side;
    Price price;
    Quantity quantity;
};

class FlowGenerator {
  public:
    FlowGenerator(std::uint64_t seed, SymbolId symbols) : rng_(seed), symbols_(symbols) {}

    [[nodiscard]] std::vector<Command> generate(std::size_t orders) {
        std::vector<Command> flow = registrations();
        if (symbols_ == 0) {
            return flow;
        }
        register_symbols(flow);
        initialize_mid_prices();
        for (std::size_t i = 0; i < orders; ++i) {
            append_next(flow);
        }
        return flow;
    }

  private:
    [[nodiscard]] std::vector<Command> registrations() const {
        std::vector<Command> flow;
        for (SymbolId s = 0; s < symbols_; ++s) {
            flow.push_back(RegisterSymbol{"S" + std::to_string(s)});
        }
        return flow;
    }

    void register_symbols(const std::vector<Command> &flow) {
        for (const auto &command : flow) {
            static_cast<void>(qsl::replay::apply(model_, command));
        }
    }

    void initialize_mid_prices() {
        mid_.resize(symbols_);
        for (SymbolId s = 0; s < symbols_; ++s) {
            mid_[s] = 100 + static_cast<Price>(s * 5);
        }
    }

    [[nodiscard]] SymbolId choose_symbol() {
        if (symbols_ == 1 || rng_() % 100 < 55) {
            return SymbolId{0};
        }
        return static_cast<SymbolId>(1 + (rng_() % (symbols_ - 1)));
    }

    void drift_mid(SymbolId symbol) {
        mid_[symbol] =
            std::clamp(mid_[symbol] + static_cast<Price>(rng_() % 3) - 1, Price{80}, Price{140});
    }

    [[nodiscard]] Price resting_price(SymbolId symbol, Side side) {
        const Price spread = 1 + static_cast<Price>(rng_() % 3);
        return side == Side::Buy ? mid_[symbol] - spread : mid_[symbol] + spread;
    }

    [[nodiscard]] Price crossing_price(SymbolId symbol, Side side) {
        const Price offset = 1 + static_cast<Price>(rng_() % 4);
        return side == Side::Buy ? mid_[symbol] + offset : mid_[symbol] - offset;
    }

    void refresh_active() {
        active_.erase(std::remove_if(active_.begin(), active_.end(),
                                     [&](const ActiveOrder &order) {
                                         return !model_.contains(order.symbol, order.id);
                                     }),
                      active_.end());
    }

    [[nodiscard]] const ActiveOrder &target_active() {
        return active_[static_cast<std::size_t>(rng_() % active_.size())];
    }

    [[nodiscard]] TimeInForce choose_tif(bool crosses) {
        if (crosses && rng_() % 100 < 35) {
            return TimeInForce::IOC;
        }
        return (rng_() % 100 < 8) ? TimeInForce::IOC : TimeInForce::GTC;
    }

    [[nodiscard]] Command make_limit(SymbolId symbol) {
        const Side side = (rng_() % 2 == 0) ? Side::Buy : Side::Sell;
        const bool crosses = rng_() % 100 < 18;
        const TimeInForce tif = choose_tif(crosses);
        const Price price = crosses ? crossing_price(symbol, side) : resting_price(symbol, side);
        const Quantity qty = static_cast<Quantity>(1 + (rng_() % 12));
        return NewLimit{symbol, next_id_++, side, price, qty, tif};
    }

    [[nodiscard]] Command make_market(SymbolId symbol) {
        const Side side = (rng_() % 2 == 0) ? Side::Buy : Side::Sell;
        const Quantity qty = static_cast<Quantity>(1 + (rng_() % 12));
        return NewMarket{symbol, next_id_++, side, qty};
    }

    [[nodiscard]] Command make_cancel() {
        const ActiveOrder &target = target_active();
        const OrderId id =
            (rng_() % 10 == 0) ? next_id_ + static_cast<OrderId>(rng_() % 5) : target.id;
        return Cancel{target.symbol, id};
    }

    [[nodiscard]] Command make_modify() {
        const ActiveOrder &target = target_active();
        const bool crosses = rng_() % 100 < 20;
        const Price price = crosses ? crossing_price(target.symbol, target.side)
                                    : resting_price(target.symbol, target.side);
        const Quantity qty = (rng_() % 100 < 45) ? static_cast<Quantity>(1 + (target.quantity / 2))
                                                 : static_cast<Quantity>(1 + (rng_() % 12));
        return Modify{target.symbol, target.id, price, qty};
    }

    [[nodiscard]] Command next_command(SymbolId symbol) {
        const int pick = static_cast<int>(rng_() % 100);
        if (active_.empty() || pick < 58) {
            return make_limit(symbol);
        }
        if (pick < 70) {
            return make_market(symbol);
        }
        if (pick < 86) {
            return make_cancel();
        }
        return make_modify();
    }

    void apply_and_track(const Command &command) {
        static_cast<void>(qsl::replay::apply(model_, command));
        const auto *limit = std::get_if<NewLimit>(&command);
        if (limit != nullptr && model_.contains(limit->symbol, limit->id)) {
            active_.push_back(
                ActiveOrder{limit->symbol, limit->id, limit->side, limit->price, limit->quantity});
        }
    }

    void append_next(std::vector<Command> &flow) {
        const SymbolId symbol = choose_symbol();
        drift_mid(symbol);
        refresh_active();
        Command command = next_command(symbol);
        flow.push_back(command);
        apply_and_track(command);
    }

    std::mt19937_64 rng_;
    SymbolId symbols_;
    MatchingEngine model_;
    std::vector<ActiveOrder> active_;
    std::vector<Price> mid_;
    OrderId next_id_ = 1;
};

} // namespace

std::vector<Command> generate_flow(std::uint64_t seed, SymbolId symbols, std::size_t orders) {
    return FlowGenerator{seed, symbols}.generate(orders);
}

std::vector<Command> generate_property_flow(std::uint64_t seed, SymbolId symbols,
                                            std::size_t orders) {
    std::mt19937_64 rng(seed);
    std::vector<Command> flow;
    for (SymbolId s = 0; s < symbols; ++s) {
        flow.push_back(RegisterSymbol{"S" + std::to_string(s)});
    }
    OrderId next_id = 1;
    for (std::size_t i = 0; i < orders; ++i) {
        // Occasionally target an unregistered symbol id (== symbols) -> UnknownSymbol.
        const auto symbol = static_cast<SymbolId>(rng() % (symbols + 1));
        const Side side = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
        // Prices mostly valid (95..105); occasionally 0 -> InvalidPrice.
        const auto price =
            (rng() % 12 == 0) ? static_cast<Price>(0) : static_cast<Price>(95 + (rng() % 11));
        // Quantities mostly 1..10; occasionally 0 (InvalidQuantity) or large (MaxQuantityExceeded).
        const std::uint64_t qr = rng() % 12;
        const auto qty = (qr == 0)   ? static_cast<Quantity>(0)
                         : (qr == 1) ? static_cast<Quantity>(50)
                                     : static_cast<Quantity>(1 + (rng() % 10));
        const TimeInForce tif = (rng() % 3 == 0) ? TimeInForce::IOC : TimeInForce::GTC;
        const int pick = static_cast<int>(rng() % 12);
        if (pick < 6) {
            // pick==5 reuses an earlier id (active -> DuplicateOrderId, inactive -> legal reuse).
            const OrderId id =
                (pick == 5 && next_id > 1) ? (1 + (rng() % (next_id - 1))) : next_id++;
            flow.push_back(NewLimit{symbol, id, side, price, qty, tif});
        } else if (pick < 7) {
            flow.push_back(NewMarket{symbol, next_id++, side, qty});
        } else if (pick < 9) {
            flow.push_back(Cancel{symbol, 1 + (rng() % next_id)}); // active or stale id
        } else {
            flow.push_back(Modify{symbol, 1 + (rng() % next_id), price, qty});
        }
    }
    return flow;
}

} // namespace qsl::replay
