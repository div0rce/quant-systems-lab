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

# a rejected NEW order (never entered the engine; lifetime-consistent with M14)
reject <id> <reason>

# final state
snapshot last_seq <n> trades <n>
sym   <id> bid <price|-> ask <price|-> orders <n>
level <sym> <B|A> <price> <qty>
```

### Notes

- **Command stream** is the authoritative replay input: registration order plus every
  submitted command. The independent OCaml engine (M16) replays these and ignores the `evt`
  lines, computing its own events/snapshot.
- **`evt` / `reject` lines** are the C++ engine's emitted outcomes, included for cross-checking
  in M17. `reject` is emitted only for rejected *new orders* (cancels/modifies of
  unknown/inactive ids are no-ops and emit nothing), consistent with the M14 lifetime model.
- **Snapshot** is the full final per-symbol state: best bid/ask, per-price aggregate levels
  (`level` lines, best-first as the engine reports them), resting order counts, last sequence
  number, and trade count.

## Determinism and consistency (tested)

`tests/unit/test_fixture_export.cpp` asserts:

- the export is byte-identical for a fixed seed, and differs across seeds;
- every line matches the grammar above (record type + arity);
- `snapshot last_seq` equals the maximum event sequence, and the reported trade count equals
  the number of `evt trade` lines;
- the flow is non-vacuous (real commands, trades, and rejections occur).

## Scope and limits

- This is a deterministic export + parseability contract, not a correctness proof and not
  formal verification.
- Independent OCaml replay (M16) and C++-vs-OCaml snapshot equality (M17) build on this schema.
