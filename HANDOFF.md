# HANDOFF.md — How to build Quant Systems Lab with Claude Code

This is the operator manual. It tells Claude Code and the human how to build the repo in a resumable, sequential, AI-first workflow.

The repo brain lives in four root files:

1. `CLAUDE.md` — durable project rules and engineering constraints.
2. `MILESTONES.md` — ordered milestone plan.
3. `PROGRESS.md` — live state and resume anchor.
4. `HANDOFF.md` — this operator manual.

Keep all four in the repo root.

---
## Current handoff

The repo is released at `v0.1.0`. M0–M28 are merged. M29 is PR #89 and should land as Linux
`perf` workflow plus constrained-environment validation only.

Current M29 state:

- Linux-only `make perf-stat` / `make perf-record` tooling exists.
- Metadata-rich `perf` artifacts exist.
- Dirty-tree handling and PMU preflight/validation exist.
- CI validates the workflow.
- The committed artifacts were generated in a constrained Docker Desktop Linux environment where
  hardware PMU counters/sampling were unavailable or permission-limited.
- The repository does **not** currently claim real hardware PMU evidence.
- Issue #90 tracks full PMU-backed evidence generation on a bare-metal or PMU-capable Linux target.

If PR #89 is still open, review/land it as constrained validation only. After PR #89 is
squash-merged, start with:

```text
/start-milestone 30
```

Do not start implementation until these files are read:

1. `CLAUDE.md`
2. `PROGRESS.md`
3. `MILESTONES.md`
4. `HANDOFF.md`

Then verify:

```bash
git status
git branch --show-current
git pull --ff-only
git log --oneline -10
gh pr list --state open
git tag -l
gh release view v0.1.0
```

Expected state after PR #89 merges:

- branch: `main`
- release tag: `v0.1.0`
- next branch: `feat/m30-socket-profiling-hardening`
- open follow-up issue: #90 for full Linux hardware PMU perf evidence

### Next milestone

M30 — Kernel/socket path profiling and Linux socket hardening. Do not implement M31–M41 inside
M30. Do not relabel constrained M29 Docker artifacts as full PMU evidence. If a PMU-capable Linux
host is available, issue #90 can be handled as the higher-priority evidence follow-up.

### Phase III / IV purpose

The current arc exists to address remaining systems-signal gaps: full hardware PMU evidence,
kernel/socket profiling, external review signal, pool-backed order-book storage integration,
advanced concurrency validation, event-driven gateway architecture, multi-client socket pressure,
NUMA/affinity studies, ingress contention, persistence/recovery benchmarking, and low-latency
networking research.

Current priority order:

1. Issue #90 — real Linux hardware PMU perf evidence.
2. M30 — socket/kernel profiling.
3. M31 — external review signal.
4. M32 — pool-backed order-book integration.
5. M33 — advanced concurrency validation.

### Forbidden shortcuts

Do not: build a dashboard; add trading strategies; add fake market-data integration; claim
production HFT; claim formal verification; add benchmark numbers without committed scripts; skip
to external review before evidence exists; or merge your own PR.

---


## 0. Use Claude Code

Build this with Claude Code, not a generic chat session.

Claude Code is the agentic coding tool that can:

- edit files,
- run git,
- run tests,
- manage branches,
- call `gh`,
- open PRs,
- resume from repo files.

Claude Code is effectively stateless across sessions, so persistence comes from files. That is why this repo uses `CLAUDE.md`, `MILESTONES.md`, `PROGRESS.md`, and `HANDOFF.md`.

If tokens run out, the next session reads those files and resumes. Civilization is mostly just logs and delusion.

---

## 1. Project summary

Project name:

```text
quant-systems-lab
```

Core artifact:

```text
Exchange Matching Engine + Market Data Feed
```

The final repo demonstrates:

1. C++20 systems programming.
2. Deterministic exchange simulation.
3. Binary protocol design.
4. Order gateway.
5. Matching engine.
6. Market data publisher.
7. Replayable event log.
8. Deterministic recovery.
9. Risk checks.
10. Latency and throughput benchmarking.
11. CI, tests, sanitizers, documentation, and clean PR history.

Target audience:

- quant SWE recruiters,
- low-latency systems engineers,
- trading infra engineers,
- C++ reviewers.

Target firms:

- Jane Street,
- Hudson River Trading,
- Citadel Securities,
- Citadel,
- Jump Trading,
- Optiver,
- IMC,
- Two Sigma,
- similar high-selectivity systems firms.

The final positioning:

> A deterministic C++20 exchange simulator with binary order gateway, price-time-priority matching engine, market-data publisher, risk checks, append-only event log, replayable recovery path, and reproducible latency/throughput benchmarks.

No fake production claims. No fake HFT claims. No trading bot cringe.

---

## 2. One-time setup

### Human setup

Install:

```bash
npm install -g @anthropic-ai/claude-code
```

Authenticate once:

```bash
claude
```

Install supporting tools:

```bash
git --version
gh --version
cmake --version
ninja --version
clang++ --version
clang-format --version
clang-tidy --version
python3 --version
```

Create an empty GitHub repo named:

```text
quant-systems-lab
```

Clone it:

```bash
git clone git@github.com:<OWNER>/quant-systems-lab.git
cd quant-systems-lab
```

Drop these four files into root:

```text
CLAUDE.md
HANDOFF.md
MILESTONES.md
PROGRESS.md
```

Commit those once on `main` only if the repo is empty and branch protection is not yet configured:

```bash
git add CLAUDE.md HANDOFF.md MILESTONES.md PROGRESS.md
git commit -m "chore: add project brain files"
git push origin main
```

After this, no more direct commits to `main`.

Enable GitHub branch protection on `main`:

- require pull request before merging,
- require status checks once CI exists,
- disallow force pushes.

---

## 3. First Claude Code prompt

Paste this into Claude Code from repo root:

```text
Read HANDOFF.md, CLAUDE.md, MILESTONES.md, and PROGRESS.md in full before doing anything.

Then bootstrap the repo for Milestone M0:
- Do not work on main.
- Start by running /start-milestone 00.
- Create the C++20/CMake scaffolding, .claude commands, settings, GitHub Actions CI, Makefile, docs skeleton, initial README, synthetic data README, results README, and initial tests.
- Implement only M0.
- Run make check.
- Update PROGRESS.md.
- Open the M0 PR.
- Stop and wait for me to squash-merge.
```

---

## 4. Resume after interruption

Open Claude Code in repo root and paste:

```text
/resume
```

It must:

1. read `CLAUDE.md`, `PROGRESS.md`, and `MILESTONES.md`,
2. run git state checks,
3. check open PRs,
4. report current milestone,
5. report next action,
6. stop and wait for confirmation.

If mid-milestone, it resumes from `PROGRESS.md` mid-milestone scratch notes and git diff.

---

## 5. Normal milestone loop

After human squash-merges a PR:

```bash
git switch main
git pull --ff-only
git branch -d feat/mNN-slug
```

Then in Claude Code:

```text
/start-milestone NN
```

Before PR:

```text
/review
```

Finish:

```text
/finish-milestone
```

Human reviews and squash-merges.

Repeat until M23.

---

## 6. Why this workflow looks human

It does not fake human development. It creates real incremental development.

The history looks clean because:

1. one feature branch per milestone,
2. one squash-merge commit per milestone,
3. Conventional Commit titles,
4. tests and docs evolve alongside code,
5. benchmark claims appear only after benchmark infrastructure exists,
6. `PROGRESS.md` records what happened.

Do not backdate commits. It is unnecessary and awkward to defend.

---

## 7. Claude Code custom commands

M0 must create these files:

```text
.claude/commands/resume.md
.claude/commands/start-milestone.md
.claude/commands/finish-milestone.md
.claude/commands/review.md
```

### `.claude/commands/resume.md`

```markdown
---
description: Re-orient after interruption and report exact next action.
allowed-tools: Read, Bash, Glob, Grep
---

Resume work on Quant Systems Lab. Do not write code yet.

1. Read CLAUDE.md, PROGRESS.md, and MILESTONES.md.
2. Run:
   - git status
   - git branch --show-current
   - git log --oneline -10
   - gh pr list
3. Determine:
   - current branch,
   - active milestone,
   - last completed milestone,
   - whether a PR is open,
   - whether the tree is clean,
   - whether make check passes if a project exists.
4. Output:
   - current state,
   - likely next action,
   - blocking issues,
   - exact milestone DoD still remaining.
5. Stop and wait for human confirmation.
```

### `.claude/commands/start-milestone.md`

```markdown
---
description: Start a milestone on a fresh feature branch. Usage: /start-milestone NN
allowed-tools: Read, Bash, Glob, Grep, Edit, Write
---

Start milestone $ARGUMENTS.

1. Read CLAUDE.md.
2. Read PROGRESS.md.
3. Read the M$ARGUMENTS section in MILESTONES.md.
4. Verify clean tree with git status.
5. If dirty, stop and ask human.
6. Run:
   - git switch main
   - git pull --ff-only
7. Create branch using exact milestone slug:
   - git switch -c feat/m$ARGUMENTS-<slug>
8. Update PROGRESS.md:
   - active milestone,
   - status in progress,
   - active branch,
   - next action.
9. Restate the DoD checklist.
10. Implement in small steps.
11. Write tests alongside code.
```

### `.claude/commands/finish-milestone.md`

```markdown
---
description: Verify, document, commit, push, and open PR for current milestone.
allowed-tools: Read, Bash, Glob, Grep, Edit, Write
---

Finish current milestone.

1. Identify current milestone from branch and PROGRESS.md.
2. Read the milestone DoD in MILESTONES.md.
3. Verify every DoD item.
4. Run make check.
5. If make check fails, fix failures before continuing.
6. Update PROGRESS.md:
   - mark milestone complete or ready for PR,
   - summarize changed files,
   - record decisions,
   - clear scratch notes.
7. Stage changes.
8. Commit using Conventional Commits.
9. Push branch:
   - git push -u origin HEAD
10. Open PR with gh pr create.
11. PR title must equal milestone PR title.
12. PR body must include:
   - summary,
   - DoD checklist,
   - tests run,
   - benchmark results if applicable,
   - limitations.
13. Output PR URL.
14. Remind human to squash-merge.
15. Do not merge.
```

### `.claude/commands/review.md`

```markdown
---
description: Strict self-review of current branch against milestone DoD.
allowed-tools: Read, Bash, Glob, Grep
---

Critically review the current branch. Do not modify files.

1. Run git diff main...HEAD.
2. Read CLAUDE.md.
3. Read current milestone DoD.
4. Check:
   - missing tests,
   - broken determinism,
   - fabricated metrics,
   - protocol undefined behavior,
   - floating point prices,
   - wall-clock dependency in core engine,
   - dead code,
   - unclear docs,
   - overclaiming,
   - benchmark claims without scripts,
   - missing PROGRESS.md update.
5. Output:
   - BLOCKING issues,
   - NON-BLOCKING issues,
   - recommended next action.
```

---

## 8. `.claude/settings.json`

M0 should create conservative project permissions:

```json
{
  "permissions": {
    "allow": [
      "Read",
      "Edit",
      "Write",
      "Glob",
      "Grep",
      "Bash(git status*)",
      "Bash(git branch*)",
      "Bash(git switch*)",
      "Bash(git checkout*)",
      "Bash(git pull*)",
      "Bash(git add*)",
      "Bash(git commit*)",
      "Bash(git diff*)",
      "Bash(git log*)",
      "Bash(gh pr*)",
      "Bash(make*)",
      "Bash(cmake*)",
      "Bash(ctest*)",
      "Bash(ninja*)",
      "Bash(clang-format*)",
      "Bash(clang-tidy*)",
      "Bash(python3*)",
      "Bash(pip*)",
      "Bash(pre-commit*)"
    ],
    "ask": [
      "Bash(git push*)"
    ],
    "deny": [
      "Read(.env)",
      "Read(secrets/**)",
      "Bash(rm -rf /)",
      "Bash(git push origin main*)",
      "Bash(git push --force*)"
    ]
  }
}
```

If Claude Code reports the settings schema has changed, inspect current Claude Code docs and update the file. Do not guess.

---

## 9. Initial Makefile contract

M0 should implement these targets or equivalent:

```makefile
.PHONY: configure build test check fmt fmt-check tidy bench asan clean

configure:
	cmake --preset dev

build:
	cmake --build --preset dev

test:
	ctest --preset dev --output-on-failure

check: fmt-check build test

fmt:
	find include src apps tests benchmarks -name '*.hpp' -o -name '*.cpp' | xargs clang-format -i

fmt-check:
	find include src apps tests benchmarks -name '*.hpp' -o -name '*.cpp' | xargs clang-format --dry-run --Werror

tidy:
	cmake --build --preset dev --target tidy

bench:
	cmake --build --preset release
	./build/release/apps/qsl-bench/qsl-bench

asan:
	cmake --preset asan
	cmake --build --preset asan
	ctest --preset asan --output-on-failure

clean:
	rm -rf build
```

Adjust paths if actual CMake layout differs.

---

## 10. GitHub PR template

M0 should create `.github/pull_request_template.md`:

```markdown
## Milestone

M__ — <name>

## Summary

<2–5 sentences>

## Definition of Done

- [ ] All DoD items from MILESTONES.md met
- [ ] make check passes
- [ ] Tests added/updated
- [ ] PROGRESS.md updated
- [ ] No benchmark claims unless measured
- [ ] No work committed directly to main

## Tests

```text
<paste commands run>
```

## Notes / decisions

<architecture decisions, limitations, follow-ups>
```

---

## 11. GitHub Actions CI

M0 should create `.github/workflows/ci.yml`:

```yaml
name: CI

on:
  pull_request:
  push:
    branches: [main]

jobs:
  build-test:
    runs-on: ubuntu-latest

    steps:
      - uses: actions/checkout@v4

      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y cmake ninja-build clang clang-tidy clang-format

      - name: Configure
        run: cmake --preset dev

      - name: Build
        run: cmake --build --preset dev

      - name: Test
        run: ctest --preset dev --output-on-failure

      - name: Format check
        run: make fmt-check
```

Add sanitizer CI later in M12.

---

## 12. README positioning

The README should open with something like:

```markdown
# Quant Systems Lab

A deterministic C++20 exchange simulator with a binary order gateway, price-time-priority matching engine, market-data publisher, append-only event log, replay/recovery path, and reproducible performance benchmarks.

This is a portfolio systems project for low-latency and quant SWE recruiting. It is not a production exchange and does not connect to real markets.
```

Avoid:

- production-grade
- institutional-grade
- battle-tested
- high-frequency trading platform
- real trading system
- guaranteed low latency

Use:

- deterministic simulator
- price-time priority
- binary protocol
- append-only event log
- replayable state
- measured benchmarks
- synthetic order flow

---

## 13. Resume bullets after completion

Use only after repo exists. Performance bullet requires measured M11 values.

### Conservative bullet

```text
Built a deterministic C++20 exchange simulator with binary order gateway, price-time-priority matching engine, market-data publisher, risk checks, append-only event log, and replayable recovery path.
```

### Performance bullet template

```text
Benchmarked synthetic order-flow scenarios at [X] orders/sec and [Y] median matching latency using reproducible CMake/Google Benchmark harness with documented hardware/compiler configuration.
```

### Systems bullet

```text
Implemented fixed-width binary protocol encoding/decoding, deterministic event sequencing, replay-based state recovery, and invariant tests covering order priority, cancellations, fills, sequence monotonicity, and book consistency.
```

---

## 14. Obsidian and external tools

Do not split the source of truth into Obsidian unless the human explicitly wants a parallel reading vault.

The canonical knowledge base is the repo:

- `CLAUDE.md`
- `HANDOFF.md`
- `MILESTONES.md`
- `PROGRESS.md`
- `docs/`
- `docs/adr/`

If using Obsidian, open the repo root or `docs/` folder as a vault. Do not maintain separate canonical notes.

Use GitHub CLI over GitHub MCP unless there is a specific reason.

Use current-docs MCP only if needed for exact Claude Code settings, CMake behavior, or library APIs.

---

## 15. Final success criteria

The repo is successful if a technical reviewer can see:

1. matching semantics,
2. integer price modeling,
3. deterministic replay,
4. explicit binary protocol boundaries,
5. C++ without obvious undefined behavior,
6. invariant testing,
7. honest benchmark methodology,
8. clean PR-based development process.

It fails if it becomes:

1. a fake trading bot,
2. a Python backtester,
3. a web dashboard first,
4. an overclaimed HFT system,
5. untested C++,
6. README performance fiction.


---

# Jane Street Internship Targeting Addendum

This section is additive. Preserve all earlier Quant Systems Lab handoff content. The project remains the same: a deterministic C++20 exchange-systems repo. This addendum adds Jane Street Hong Kong December–February internship targeting context and additional execution requirements.

## Role ranking for this user

Use this project primarily for:

1. **Jane Street Software Engineer Internship — Hong Kong — December–February**
2. **Jane Street Linux Engineer Internship — Hong Kong — December–February**

Optional but lower priority:

3. Strategy and Product Internship
4. IT Operations Engineer Internship

Do not optimize engineering work for IT Operations. That path risks labeling the user as support/sysadmin rather than software engineer. The project should scream software/systems competence.

## Why SWE is primary

The SWE role values:

- programming ability;
- learning speed;
- maintainable high-quality code;
- production-intended engineering;
- exposure to OCaml;
- high-performance trading systems;
- programming-language-adjacent thinking.

The existing C++ exchange simulator maps to high-performance systems. The added OCaml replay verifier maps to Jane Street language culture.

## Why Linux Engineering is secondary but realistic

The Linux role values:

- OS fundamentals;
- computer architecture;
- network protocols;
- command line usage;
- C/sockets/virtual memory/process lifecycle;
- production infrastructure;
- debugging and root-cause analysis;
- automation.

Quant Systems Lab should therefore include:

- TCP gateway;
- socket docs;
- malformed-frame handling;
- replay/recovery CLI;
- benchmark scripts;
- Linux performance notes;
- deterministic root-cause-friendly event logs.

## Additive milestone requirement

Add **M14 — OCaml replay verifier** after final docs/hardening or as a late-stage dedicated milestone. Do not pull it into early C++ milestones.

M14 exists to produce a targeted Jane Street SWE signal without corrupting the main C++ project.

## How Claude Code should present the project to recruiters

### For Jane Street SWE

Use title:

```text
Quant Systems Lab — C++20 Exchange Simulator + OCaml Replay Verifier
```

Use bullets:

```text
- Built a deterministic C++20 exchange simulator with binary order gateway, price-time-priority matching engine, market-data publisher, risk checks, append-only event log, and replayable recovery path.
- Implemented fixed-width protocol encoding/decoding, deterministic sequencing, and invariant tests covering fills, cancellations, priority preservation, malformed frames, and replay equivalence.
- Added an OCaml replay verifier using immutable state transitions to validate exported engine logs and final book snapshots.
```

### For Jane Street Linux Engineering

Use title:

```text
Quant Systems Lab — Linux Systems + Exchange Infrastructure Simulator
```

Use bullets:

```text
- Built a Linux-focused C++20 exchange infrastructure simulator with TCP order gateway, binary protocol framing, deterministic event logs, replay recovery, and reproducible benchmark tooling.
- Implemented socket-based client/server tooling, malformed-frame handling, append-only log inspection, and recovery utilities for debugging deterministic order-flow scenarios.
- Documented systems tradeoffs across protocol design, process boundaries, replayability, benchmark methodology, and Linux performance measurement.
```

## Added target repo paths

Add the following paths by the appropriate milestones:

```text
docs/linux_performance.md
docs/socket_gateway.md
ocaml/
  dune-project
  bin/verify_replay.ml
  lib/event.ml
  lib/parser.ml
  lib/replay.ml
  lib/invariant.ml
  test/test_replay.ml
```

## How to start from scratch

Paste into Claude Code:

```text
Read HANDOFF.md, CLAUDE.md, MILESTONES.md, and PROGRESS.md in full before doing anything.

This repo is now targeted at Jane Street SWE and Linux Engineering internships. Preserve the original C++ exchange-systems scope, and also preserve the additive Jane Street context: Linux docs, socket credibility, benchmark honesty, and the late OCaml replay verifier milestone.

Then bootstrap Milestone M0 only:
- do not work on main;
- run /start-milestone 00;
- create the C++20/CMake scaffolding, Claude Code commands, settings, CI, Makefile, docs skeleton, and initial tests;
- include placeholder docs for linux_performance.md and socket_gateway.md if M0 scope permits without bloat;
- do not implement engine logic yet;
- run make check;
- update PROGRESS.md;
- open the M0 PR;
- stop and wait for me to squash-merge.
```

## How to resume

```text
/resume
```

Claude Code must reconstruct state from files and git. Do not rely on chat memory. If mid-milestone, update or read the Mid-milestone scratch section in PROGRESS.md.

## Extra review checklist for Jane Street alignment

Before every PR, check:

1. Does this make the repo more credible for SWE or Linux Engineering?
2. Did we accidentally drift into fake trading-bot territory?
3. Are all benchmark claims measured?
4. Are C++ APIs small and testable?
5. Is deterministic replay preserved?
6. Are protocol boundaries explicit?
7. Are Linux/socket concepts documented honestly?
8. Is OCaml isolated to the replay verifier, not mixed into the core engine prematurely?


## Additive M15–M20 technical roadmap replacing old optional application polish

The prior optional `M15 — Jane Street application polish` milestone is removed. Do not implement recruiter-facing polish as the next milestone. The project should now continue with technical depth:

1. **M15 — Export normalized command streams + final snapshots**
   - Export complete command streams, engine events, rejections, symbol registration order, and final per-symbol snapshots.
   - This gives the OCaml side enough information to replay independently.
2. **M16 — Independent OCaml replay engine**
   - OCaml replays the command stream immutably and computes its own final snapshot.
   - It must not merely inspect the C++ event log.
3. **M17 — Differential replay tests: C++ vs OCaml snapshot equality**
   - CI compares C++ exported snapshots against OCaml-computed snapshots.
4. **M18 — Property-based command generator**
   - Generate seeded randomized command streams covering valid, invalid, duplicate, reused, IOC, market, cancel, modify, and multi-symbol cases.
5. **M19 — Shrinker + minimal failing fixture exporter**
   - Reduce failing generated streams to small, replayable counterexamples.
6. **M20 — Final docs: differential testing architecture**
   - Document the architecture, fixture schemas, property generator, shrinker, and exact limits.

### Strategic reason

The repo should not stop at “built a matching engine.” That is a known portfolio project. The stronger claim is: built a deterministic exchange simulator plus a cross-language differential testing system that can generate, replay, compare, and shrink market-state counterexamples.

### Non-negotiable constraints

- Do not remove existing context or prior milestone history.
- Do not overclaim formal verification.
- Do not claim production exchange behavior.
- Do not claim trading profitability.
- Do not add application-polish docs unless M20 explicitly needs final technical framing.
- Keep C++ as the system under test and OCaml as independent replay/checking infrastructure.
- Every benchmark or performance claim must remain measured by committed scripts.
