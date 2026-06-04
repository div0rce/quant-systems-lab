---
description: Re-orient after interruption and report exact next action.
allowed-tools: Read, Bash, Glob, Grep
---

Resume work on Quant Systems Lab. Do not write code yet.

1. Read CLAUDE.md, PROGRESS.md, MILESTONES.md, and HANDOFF.md.
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
