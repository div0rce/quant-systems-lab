# Data

Scaffold directory for synthetic input data. **No real or synthetic market data is committed
here.** Synthetic order flows are generated deterministically by the C++ fixture tooling, and
the committed fixtures live under `ocaml/test/fixtures/` and `regressions/`. See
`data/synthetic/README.md`. All data in this project is synthetic; it does not use real market
data.
