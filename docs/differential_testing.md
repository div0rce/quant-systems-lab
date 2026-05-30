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
