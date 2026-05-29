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
