# Matching Rules

> Placeholder — implemented in M3.

## Price-time priority

- Buy orders match when `buy_price >= best_ask`
- Sell orders match when `sell_price <= best_bid`
- Within a price level, earlier orders have priority
- Partial fills preserve remaining order priority
- Modified orders lose priority if price or quantity increases
- Quantity reduction preserves priority
