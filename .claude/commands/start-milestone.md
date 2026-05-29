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
