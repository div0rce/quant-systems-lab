# Differential Testing — fixture schema (M15)

This is the first step of the Phase II differential-testing roadmap (M15–M20). The C++ engine
is the system under test; the OCaml side (added in M16) will independently replay the command
stream and compare its computed snapshot against the C++ snapshot exported here (M17).

M15 only **exports** the data and proves it is deterministic and parseable. It does not add the
OCaml replay engine, property-based generation, or shrinking.

## Producing a fixture

```bash
cmake --build --preset dev --target qsl-export-stream
./build/dev/qsl-export-stream [seed] [orders] > fixture.txt
```

The exporter drives the deterministic synthetic flow (`generate_flow(seed)`) through the risk
gateway and is byte-for-byte reproducible for a given seed. A committed example lives at
`ocaml/test/fixtures/stream_seed7.txt`.

## Format (version 1)

Line-oriented, space-separated tokens, big-endian-agnostic plain text. Lines starting with `#`
are comments. Integers are decimal; a missing best price is `-`. Sides are `B`/`S`; TIF is
`GTC`/`IOC`.

```text
version 1
meta seed <s> symbols <n> orders <n> max_qty <q> max_notional <v>

# command stream (in submission order), each followed by its outcome lines
cmd reg <name>
cmd limit  <sym> <id> <B|S> <price> <qty> <GTC|IOC>
cmd market <sym> <id> <B|S> <qty>
cmd cancel <sym> <id>
cmd modify <sym> <id> <price> <qty>

# engine events emitted by the preceding command (sequence order)
evt accept <seq> <sym> <id>
evt cancel <seq> <sym> <id>
evt modify <seq> <sym> <id>
evt trade  <seq> <sym> <taker> <maker> <price> <qty>

# a gateway/risk rejection (never entered the engine event stream)
reject <new_limit|new_market|cancel|modify> <id> <reason>

# final state
snapshot last_seq <n> trades <n>
sym   <id> bid <price|-> ask <price|-> orders <n>
level <sym> <B|A> <price> <qty>
```

### Notes

- **Command stream** is the authoritative replay input: registration order plus every
  submitted command. The independent OCaml engine (M16) replays these and ignores the `evt`
  lines, computing its own events/snapshot.
- **`evt` / `reject` lines** are the C++ gateway/engine outcomes, included for cross-checking
  in M17. `evt` lines are sequenced engine events. `reject` lines are structured gateway/risk
  rejections scoped by command kind; rejected commands never enter the engine event stream and
  do not consume sequence numbers.
- Accepted commands may emit zero or more `evt` lines. Unknown-order no-ops may emit no event
  and no rejection, so M17 must not assume every command has an outcome line. Explicit `reject`
  lines are part of the C++ outcome stream and distinguish risk/gateway rejection from no-op
  commands.
- **Snapshot** is the full final per-symbol state: best bid/ask, per-price aggregate levels
  (`level` lines, best-first as the engine reports them), resting order counts, last sequence
  number, and trade count. Each `level` line carries its symbol explicitly; the OCaml parser
  validates that the level symbol matches the surrounding `sym` block. Malformed snapshot
  ownership is rejected rather than normalized away.

## Determinism and consistency (tested)

`tests/unit/test_fixture_export.cpp` asserts:

- the export is byte-identical for a fixed seed, and differs across seeds;
- every line matches the grammar above (record type + arity), including command-scoped
  rejection lines and parseable risk metadata;
- `snapshot last_seq` equals the maximum event sequence, and the reported trade count equals
  the number of `evt trade` lines;
- registered-but-empty symbols remain present in the snapshot with no `level` records;
- the flow is non-vacuous (real commands, trades, and rejections occur).

## Scope and limits

- This is a deterministic export + parseability contract, not a correctness proof and not
  formal verification.
- Independent OCaml replay (M16) and C++-vs-OCaml snapshot equality (M17) build on this schema.


## M17 — differential replay (C++ vs OCaml)

`ocaml/test/test_differential.ml` closes the loop: for each fixture it independently replays the
command stream in OCaml (`Replay_engine`), then compares the OCaml-computed snapshot against the
C++ snapshot embedded in the same fixture. Equality covers per-symbol best bid/ask, per-price
level aggregates, resting order counts, `last_seq`, and trade count (compared via the canonical
`snapshot_to_lines` rendering, so a mismatch prints a readable `computed` vs `expected` line).

Fixtures under test:

- `stream_seed7.txt` — the synthetic flow (GTC limits, market, cancel, modify, rejects, 4 symbols);
- `stream_ioc.txt` — a hand-built scenario from `qsl-export-stream ioc` covering IOC discard
  (partial and no-cross), market, and partial-maker fills (the synthetic flow uses only GTC);
- `stream_bad_snapshot.txt` — a valid command stream with a deliberately corrupted snapshot
  section; the test asserts the mismatch **is** detected.
- `bad_snapshot_level_symbol.txt` — a deliberately malformed snapshot where a `level` record
  claims a different symbol than the surrounding `sym` block; the parser rejects it before
  comparison, so malformed per-symbol ownership cannot be normalized into equality.

The check runs under the existing `ocaml-verifier` CI job via `dune runtest` (no separate job).
It is differential testing against the C++ system under test, not formal verification.


## M18 — property-based command generator

`generate_property_flow(seed)` (C++) produces an enriched, seed-deterministic command stream
that deliberately exercises the full command space: valid limit/market orders, IOC, invalid
prices and quantities, duplicate active ids, reused inactive ids, unknown symbols, cancels and
modifies of active and inactive orders, and multi-symbol interleavings. `qsl-export-stream prop
<seed>` exports one fixture per seed; `prop_seed1..8.txt` are committed.

`test_differential.ml` discovers every `prop_*.txt` fixture (via `Sys.readdir`) and runs the
same C++-vs-OCaml snapshot equality plus a no-crossed-book invariant on each, reporting the
failing fixture/seed on divergence. Across seeds 1–8 the two engines agree exactly while
exercising all reject reasons (UnknownSymbol, UnknownOrder, InvalidPrice, InvalidQuantity,
MaxQuantityExceeded, MaxNotionalExceeded, DuplicateOrderId) and real trades.

### Oracle hardening

- **Negative coverage:** three hand-corrupted fixtures (`stream_bad_snapshot`,
  `stream_bad_lastseq`, `stream_bad_orders`) corrupt distinct snapshot fields (an ask level,
  `last_seq`, and `order_count`); the test asserts each is detected, proving the comparison
  is not blind to those fields.
- **Golden regeneration:** `make check-fixtures` (`scripts/check_fixtures.sh`, run in the
  `build-test` CI job) regenerates every C++-produced fixture and diffs it against the
  committed copy, so the differential tests can never silently compare OCaml against a stale
  C++ snapshot. Hand-authored `stream_bad_*` fixtures are intentionally not regenerated.

This is property-based differential testing against the C++ system under test — not formal
verification or a proof of correctness.


## M19 — shrinker + minimal failing fixtures

`replay::shrink(commands, predicate)` (C++) reduces a failing command stream to a small,
reviewable counterexample while preserving a failure predicate. It is greedy and deterministic,
iterating three strategies to a fixed point: remove contiguous chunks (decreasing size), remove
single commands, and simplify fields (lower quantities). `qsl-export-stream shrink <seed>`
shrinks the property flow for a seed and writes the minimized differential fixture prefixed
with a shrink report (seed, original length, minimized length, failure reason).

The committed `shrunk_seed1.txt` reduces a 123-command flow to **5** commands (three symbol
registrations + a resting sell + a crossing IOC buy that trades), and the OCaml differential
test replays it independently.

### Limitations (honest)

- **Artificial predicate.** The real predicate would be "C++ and OCaml snapshots disagree", but
  the engines currently agree on every tested stream, so the demonstrated predicate is the
  artificial "produces a trade". The shrinker is predicate-agnostic; a divergence predicate
  plugs in unchanged.
- **Greedy, not globally minimal.** It finds a 1-minimal stream under removal, not the smallest
  possible counterexample.
- **Field simplification is limited to lowering quantities.** Prices and symbol/order ids are
  reduced only indirectly, via command removal.
- **No symbol/id renumbering.** Symbol ids are assigned by registration order, so a registration
  referenced by a surviving order cannot be removed (removing it would renumber ids and break
  the stream) — which is why three registrations remain in `shrunk_seed1.txt`.
- This is shrinking for differential/property testing, not a proof of minimality or correctness.
