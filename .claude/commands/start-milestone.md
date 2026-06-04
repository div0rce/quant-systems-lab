---
description: Start a milestone on a fresh feature branch. Usage: /start-milestone NN
allowed-tools: Read, Bash, Glob, Grep, Edit, Write
---

Start milestone $ARGUMENTS.

1. Verify clean tree with git status.
2. If dirty, stop and ask human.
3. Run:
   - git switch main
   - git pull --ff-only
4. Read updated CLAUDE.md.
5. Read updated PROGRESS.md.
6. Read updated HANDOFF.md.
7. Read the M$ARGUMENTS section in updated MILESTONES.md.
   - Read the selected milestone entry in `MILESTONES.md`. Use its exact `Branch:` value. Do not synthesize `feat/mNN-*` unless that is the exact branch listed. If no branch is listed, stop and ask.
8. Create the branch using exactly the milestone's `Branch:` value:
   - git switch -c <exact Branch value from MILESTONES.md>
   - Do not infer a prefix from the milestone number.
   - Do not silently create a branch that violates `MILESTONES.md`.
9. Update PROGRESS.md:
   - active milestone,
   - status in progress,
   - active branch,
   - next action.
10. Restate the DoD checklist.
11. Implement in small steps.
12. Write tests alongside code.
