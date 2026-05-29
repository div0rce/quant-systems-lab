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
