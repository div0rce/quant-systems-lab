# OCaml Replay Verifier

An independent replay-invariant checker for exported exchange event logs, written in OCaml
(`ocaml/`). It is deliberately small and external: it does **not** re-implement the matching
engine and it does **not** prove the engine correct. It re-derives a set of replay invariants
from a normalized event-log fixture and reports pass/fail.

## Why a second language

The C++ tests and the engine share code and assumptions; a bug in a shared assumption can hide
from tests written against the same model. A verifier written independently, in a typed
functional language with immutable data, is a cheap cross-check: it parses the *output* of the
C++ pipeline and validates properties that must hold regardless of how the engine is
implemented. The signal is an independent checker in a typed functional language — not a claim
of OCaml mastery or formal verification.

## Fixture format

`qsl-export-fixture [seed] [orders]` drives a deterministic synthetic flow through the risk
gateway and writes a textual, line-oriented fixture to stdout. A low `max_order_quantity`
makes some new orders reject, so the fixture exercises the rejection invariant.

```text
# comment
v 1
meta seed 42 symbols 4 orders 200 max_qty 8
reject <order_id> <reason>            # a new order rejected by risk (never reached the engine)
accept <seq> <symbol> <order_id>      # engine lifecycle events, in sequence order
cancel <seq> <symbol> <order_id>
modify <seq> <symbol> <order_id>
trade  <seq> <symbol> <taker> <maker> <price> <qty>
summary last_seq <n> trades <n>       # engine-reported totals
```

## Invariants checked

Each is recomputed from the raw records, independently of the engine:

1. **Sequence strictly increasing** — event sequence numbers are monotonic.
2. **Positive trade quantity** — no zero/negative trade quantities.
3. **Canceled order cannot later trade** — an id seen in a `cancel` never appears as taker or
   maker in a later `trade`.
4. **Rejected order never rests or trades** — a rejected new-order id never appears in any
   engine event.
5. **Summary matches event log** — the reported `last_seq` equals the maximum event sequence
   and the reported trade count equals the number of `trade` records.

## Scope and limitations

- This checks **invariants over the exported log**, not full book-state re-computation. It does
  not independently re-run price-time matching, so it cannot by itself confirm best bid/ask or
  resting quantities — those are covered on the C++ side (`docs/invariants.md`, replay-equivalence
  tests). The OCaml side ties the event log to the engine's reported summary.
- It is **not** formal verification and makes **no** correctness proof; it is reproducible,
  deterministic invariant checking on fixed-seed fixtures.

## Build and run

Local toolchain: OCaml + dune (e.g. `brew install ocaml dune`; no opam required — only the
standard library is used).

```bash
# regenerate the committed fixture from the C++ pipeline
cmake --build --preset dev --target qsl-export-fixture
./build/dev/qsl-export-fixture 42 200 > ocaml/test/fixtures/valid.txt

# build, test, and run the verifier
cd ocaml
dune build
dune runtest                                   # checks valid + violation fixtures
dune exec bin/verify_replay.exe -- test/fixtures/valid.txt
```

CI runs `dune build` and `dune runtest` in a dedicated `ocaml-verifier` job (installs `ocaml`
and `dune` via apt; no opam).
