---
name: Technical review / criticism
about: Report a correctness, methodology, or clarity problem (see docs/review_request.md)
title: "[review] <area>: <short summary>"
labels: review
---

Thanks for reviewing Quant Systems Lab. This project actively wants **adversarial** criticism —
the goal is to find what is wrong, not to confirm what looks right. See
[`docs/review_request.md`](../../docs/review_request.md) for the areas where review is most
valuable, and note that **no external review has happened yet** (every claim is self-certified).

## Area

<!-- One of: SPSC memory ordering / backpressure semantics / threaded ownership model /
event-log integrity under concurrency / benchmark + profiling methodology /
Linux + socket profiling methodology / other. -->

## What is wrong or unclear

<!-- The specific claim, file, or line — and why it is incorrect, under-justified, or misleading. -->

## Evidence / reproduction (optional)

<!-- Commands, a counterexample, a citation, or the reasoning that supports the criticism. -->

## Suggested fix (optional)

<!-- If you have one. A PR is also very welcome. -->

---

Context: this repo is a deterministic exchange **simulator** and a portfolio systems project —
**not** a production exchange, and it makes no latency, throughput, or profitability claims.
Accepted and rejected feedback is recorded in
[`docs/review_feedback.md`](../../docs/review_feedback.md).
