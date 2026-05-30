#include "qsl/replay/recovery.hpp"

#include <random>
#include <string>

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
        if (record.type != RecordType::Command) {
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

std::vector<Command> generate_flow(std::uint64_t seed, SymbolId symbols, std::size_t orders) {
    std::mt19937_64 rng(seed);
    std::vector<Command> flow;
    for (SymbolId s = 0; s < symbols; ++s) {
        flow.push_back(RegisterSymbol{"S" + std::to_string(s)});
    }
    OrderId next_id = 1;
    for (std::size_t i = 0; i < orders; ++i) {
        const auto symbol = static_cast<SymbolId>(rng() % symbols);
        const Side side = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
        const auto price = static_cast<Price>(95 + (rng() % 11)); // 95..105
        const auto qty = static_cast<Quantity>(1 + (rng() % 10)); // 1..10
        const int pick = static_cast<int>(rng() % 10);
        if (pick < 6) {
            flow.push_back(NewLimit{symbol, next_id++, side, price, qty, TimeInForce::GTC});
        } else if (pick < 7) {
            flow.push_back(NewMarket{symbol, next_id++, side, qty});
        } else if (pick < 9) {
            flow.push_back(Cancel{symbol, 1 + (rng() % next_id)}); // may target a stale id
        } else {
            flow.push_back(Modify{symbol, 1 + (rng() % next_id), price, qty});
        }
    }
    return flow;
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
