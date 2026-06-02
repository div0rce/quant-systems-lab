#!/usr/bin/env bash
# Opt-in long concurrency validation loop. This is intentionally not part of normal CI: it repeats
# the concurrency-labelled tests to broaden schedule coverage when a developer wants a longer run.
set -euo pipefail

cd "$(dirname "$0")/.."

PRESET="${QSL_CONCURRENCY_STRESS_PRESET:-dev}"
LOOPS="${QSL_CONCURRENCY_STRESS_LOOPS:-25}"
LABEL="${QSL_CONCURRENCY_STRESS_LABEL:-concurrency}"

case "$LOOPS" in
    ''|*[!0-9]*)
        echo "error: QSL_CONCURRENCY_STRESS_LOOPS must be a positive integer" >&2
        exit 2
        ;;
esac

if [[ "$LOOPS" -lt 1 ]]; then
    echo "error: QSL_CONCURRENCY_STRESS_LOOPS must be >= 1" >&2
    exit 2
fi

cmake --preset "$PRESET"
cmake --build --preset "$PRESET"

echo "concurrency stress: preset=$PRESET label=$LABEL loops=$LOOPS"
i=1
while [[ "$i" -le "$LOOPS" ]]; do
    echo "concurrency stress loop $i/$LOOPS"
    ctest --preset "$PRESET" -L "$LABEL" --output-on-failure
    i=$((i + 1))
done
