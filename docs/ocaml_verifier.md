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
3. **Canceled order cannot later trade** — an id is canceled only for its *current* lifetime;
   trading it counts as a violation only if there is no later `accept` re-establishing it.
4. **Rejected order never rests or trades** — a rejected attempt never entered the engine, so
   the id must not rest (`cancel`/`modify`) or trade *until* a later `accept` reuses it.
5. **Summary matches event log** — the reported `last_seq` equals the maximum event sequence
   and the reported trade count equals the number of `trade` records.

## OrderId lifetimes

OrderId uniqueness in this system is scoped to currently-active resting orders, **not** to
global history:

- A rejected attempt never enters the engine, so the same numeric id may later be submitted
  and accepted — the rejected attempt does not permanently tombstone the id.
- A canceled (or fully-filled) order leaves the active set, so its id may be reused by a later
  accept, which begins a new valid lifetime.

The verifier therefore tracks per-id lifetime state in sequence order: an `accept` starts a
fresh active lifetime that clears any prior rejected/canceled state, and the canceled/rejected
checks only fire against the id's *current* lifetime. This keeps the verifier from rejecting
logs that are valid under the engine's active-order semantics.

## Independent replay engine (M16)

Beyond the log-invariant checker above, `ocaml/lib/replay_engine.ml` is an **independent**
matching engine: it consumes an M15 command-stream fixture (`stream_parser.ml` parses the
`meta` risk config and `cmd` lines, ignoring the C++ `evt`/`snapshot` output) and replays it
immutably to compute its own final snapshot — it does not trust the C++ engine's emitted
events. It mirrors the C++ semantics so the snapshots can be compared:

- integer ticks; bids best = highest, asks best = lowest; FIFO within a level; fills at the
  resting maker's price;
- GTC rests the remainder, IOC discards it, market orders never rest;
- gateway risk (unknown symbol → duplicate active id → value/quantity/notional checks) gates
  the engine; OrderId uniqueness is active-lifetime scoped;
- every registered symbol appears in the snapshot;
- sequence numbers count emitted events, so `last_seq` matches the C++ engine.

`replay_snapshot <fixture>` prints the OCaml-computed snapshot. On the committed
`stream_seed7.txt` it independently reproduces the C++ result (`last_seq 47`, `trades 13`, and
the same per-symbol best bid/ask and order counts). The **automated** C++-vs-OCaml snapshot
equality check (with readable diffs, in CI) is M17; M16 provides the engine and unit tests
covering matching, partial fills, cancel, modify (in-place and repricing), IOC, market,
duplicate id, risk rejection, id reuse, and the empty-registered-symbol contract.

## Oracle independence audit (#39)

A differential test is only as strong as the oracle's independence: if the OCaml engine merely
transcribes C++ logic, agreement proves nothing. This is an honest audit of where
`replay_engine.ml` is genuinely independent and where it deliberately *mirrors* the C++ system
(so a shared assumption could hide from the differential).

**Genuinely independent (a behavioural divergence here is caught):**

- **Different language and data model.** The oracle stores each side as an immutable
  `Map.Make(Int)` from price to a FIFO `order list`, with a separate `id -> (side, price)`
  location map; the C++ engine uses a mutable intrusive book. Matching is a pure recursive fold
  (`match_qty`/`apply`), not a translation of C++ pointer manipulation. A data-structure,
  aliasing, or iterator-invalidation bug on one side surfaces as a final-state difference.
- **Replays commands, not events.** `stream_parser` feeds the oracle the `meta` + `cmd` lines
  and *ignores* the C++ `evt`/`snapshot` output; the oracle re-derives state from inputs rather
  than trusting the engine's emitted events.
- **Parameterised risk limits.** `max_qty`/`max_notional` come from the fixture `meta`, not
  hard-coded constants.

**Deliberately mirrored (a *shared* error here would NOT be caught):**

- **Integer notional rule.** `check_limit_values` rejects when `qty > max_notional / price`
  using truncating integer division — the exact C++ formula. A shared off-by-one in this
  truncation is invisible to the differential. *(Highest-risk mirror.)*
- **Modify priority rule.** `modify_book` keeps queue priority only for a same-price reduction
  (`new_price = price0 && new_qty <= resting_qty`), treats a price change or quantity increase
  as cancel + re-add (which may cross), and treats `qty = 0` as cancel. This subtle rule is
  matched to C++ by construction.
- **Sequence-number accounting.** `put_book` advances `seq` by `1 + #trades` on accept,
  `1` on cancel, `1 + #trades` on modify, and `0` on reject. `last_seq` is compared, so a
  divergence is caught — but the *rule itself* was written to match, not re-specified.
- **Validation gating and snapshot shape.** The check order (unknown symbol → duplicate active
  id → value checks), the active-lifetime id scoping, and "every registered symbol appears,
  ordered by id ascending" are all mirrored conventions.

**Out of differential scope by construction:**

- **Reject *reasons*.** Any reject produces no state change and consumes no seq, so the snapshot
  cannot distinguish `InvalidPrice` from `MaxNotionalExceeded`. Reason coverage lives in the C++
  unit tests, not the differential.
- **Snapshot serialization.** `snapshot_to_lines` is hand-matched to the C++ exporter's line
  format; the comparison is on rendered lines. A pure formatting divergence is guarded
  separately by the golden regeneration check (`make check-fixtures`), not by the oracle.
- **The specification itself.** Where both implementations faithfully encode the same documented
  rule (`docs/matching_rules.md`), an error in that rule is not detectable by agreement.

**Bottom line.** The differential catches transcription, logic, and data-structure bugs that
make the two engines compute different state from the same commands. It does not, and is not
claimed to, catch shared-specification errors, serialization-format errors (golden-guarded), or
reject-reason differences (unit-tested). The mirrored items above are the deliberate cost of a
snapshot-equality oracle.

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
