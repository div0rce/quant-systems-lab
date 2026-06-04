# Contributing

This is a single-maintainer portfolio systems project. Contributions and issues are welcome,
but the bar is deliberately strict: changes should keep the repo deterministic, honest, and
reviewable.

## Workflow

- **Never work on `main`.** All work happens on a scoped branch — milestone branches use the
  prefix that describes the work (`feat/mNN-slug`, `test/mNN-slug`, `docs/mNN-slug`,
  `perf/mNN-slug`, or `refactor/mNN-slug`), while backlog issue branches use names such as
  `feat/issue-NN-slug` or `fix/issue-NN-slug`. Use one scoped change per branch, merged via a
  single squash-merge PR.
- Use [Conventional Commits](https://www.conventionalcommits.org/) (`feat:`, `fix:`, `test:`,
  `docs:`, `chore:`, `perf:`, `ci:`, `refactor:`).
- Keep PRs small, scoped, and reviewable. Move follow-ups to the backlog in `MILESTONES.md`
  rather than expanding a PR.

## Checks (run before opening a PR)

```bash
make check                 # clang-format check + build + tests
make asan                  # AddressSanitizer + UBSan build and tests
dune runtest --root ocaml  # OCaml log verifier + independent replay + differential + mutation tests
```

If you change the C++-generated differential fixtures, also run `make check-fixtures` (it
regenerates them and fails on drift) and `make check-manifest` (it verifies the provenance
manifest). `make determinism` asserts the fixtures are byte-identical across gcc and clang.
Benchmarks are reproduced with `make bench` (core) and `make bench-diff` (differential harness);
`make divergence-demo` exercises the shrinker on an injected divergence. Linux/socket evidence is
reproduced with the relevant opt-in targets, such as `make profile-io`, `make socket-stress`, and
`make socket-load`, where the host supports them.

## Determinism and benchmarks

- Core matching is deterministic and must not depend on wall-clock time; prices are integer
  ticks (never floating point).
- **No fabricated performance claims.** Benchmark numbers may only come from the committed
  harness (`make bench`), must be reproducible from it, and must carry their hardware/compiler/
  build context. Do not hand-edit numbers into the README or docs.
- Run `make bench` only when intentionally updating benchmark results.

## Scope

Keep changes within the project's scope (a deterministic exchange-systems lab). It is not a
production exchange, a trading strategy, or connected to real markets — see `README.md`.
