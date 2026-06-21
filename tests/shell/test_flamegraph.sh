#!/usr/bin/env bash
# Unit tests for scripts/flamegraph.py — the dependency-free stackcollapse + SVG
# renderer behind `make flamegraph` (issue #32).
#
# The shell driver (scripts/flamegraph.sh) needs Linux `perf`, which CI does not
# have, so these tests exercise the deterministic, portable core instead:
#   1. `perf script` output folds into correct collapsed stacks (innermost-first
#      perf order reversed to root-first, comm at the base, dso + "+0xoffset"
#      stripped, C++ symbols with spaces/parens preserved).
#   2. identical stacks aggregate their counts.
#   3. collapsed output is sorted and deterministic.
#   4. the SVG render is well-formed, escapes XML metacharacters, contains the
#      expected frames, and is byte-identical across runs (no RNG, no timestamps).
#   5. empty input is handled (exit 1 for SVG, empty for --collapse-only).
#
# Registered with CTest (see tests/CMakeLists.txt); runs under `make check`.
# Run directly: bash tests/shell/test_flamegraph.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FG="$REPO_ROOT/scripts/flamegraph.py"

if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 not found; flamegraph renderer tests skipped"
    exit 0
fi

PASS=0
FAIL=0

expect_eq() {
    local name="$1" expected="$2" actual="$3"
    if [[ "$actual" == "$expected" ]]; then
        printf 'PASS: %s\n' "$name"
        PASS=$((PASS + 1))
    else
        printf 'FAIL: %s\n      expected: %q\n      actual:   %q\n' "$name" "$expected" "$actual"
        FAIL=$((FAIL + 1))
    fi
}

expect_contains() {
    local name="$1" needle="$2" haystack="$3"
    if [[ "$haystack" == *"$needle"* ]]; then
        printf 'PASS: %s\n' "$name"
        PASS=$((PASS + 1))
    else
        printf 'FAIL: %s\n      missing: %q\n' "$name" "$needle"
        FAIL=$((FAIL + 1))
    fi
}

expect_not_contains() {
    local name="$1" needle="$2" haystack="$3"
    if [[ "$haystack" != *"$needle"* ]]; then
        printf 'PASS: %s\n' "$name"
        PASS=$((PASS + 1))
    else
        printf 'FAIL: %s\n      unexpected: %q\n' "$name" "$needle"
        FAIL=$((FAIL + 1))
    fi
}

# Build a synthetic `perf script` block. Frame lines must start with a TAB; the
# header line for each sample must start in column 0.
TAB=$'\t'
make_perf_script() {
    printf '%s\n' \
        "qsl-bench 100 1.0: 1000 cpu-clock:u:" \
        "${TAB}415cd0 qsl::engine::OrderBook::add_limit(unsigned long, qsl::core::Side)+0x310 (/path/qsl-bench)" \
        "${TAB}402887 main+0x127 (/path/qsl-bench)" \
        "" \
        "qsl-bench 100 2.0: 1000 cpu-clock:u:" \
        "${TAB}415cd0 qsl::engine::OrderBook::add_limit(unsigned long, qsl::core::Side)+0x300 (/path/qsl-bench)" \
        "${TAB}402887 main+0x100 (/path/qsl-bench)" \
        "" \
        "qsl-bench 100 3.0: 1000 cpu-clock:u:" \
        "${TAB}aaaa cfree+0x5 (/usr/lib64/libc.so.6)" \
        "${TAB}402887 main+0x10 (/path/qsl-bench)" \
        ""
}

# --- Folding (stackcollapse) ------------------------------------------------

FOLDED="$(make_perf_script | python3 "$FG" --collapse-only)"

# Innermost-first perf order is reversed to root-first, comm prepended, dso and
# "+0xoffset" stripped. The two add_limit samples (different offsets) collapse to
# one stack with count 2.
expect_contains "add_limit stack folds with comm at base, offset+dso stripped, count 2" \
    'qsl-bench;main;qsl::engine::OrderBook::add_limit(unsigned long, qsl::core::Side) 2' \
    "$FOLDED"
expect_contains "libc leaf folds to one sample" \
    'qsl-bench;main;cfree 1' \
    "$FOLDED"
expect_not_contains "dso paths are stripped from frames" "/usr/lib64/libc.so.6" "$FOLDED"
expect_not_contains "raw +0x offsets are stripped from frames" "+0x" "$FOLDED"

# Collapsed output is sorted (deterministic) and stable across runs.
FOLDED2="$(make_perf_script | python3 "$FG" --collapse-only)"
expect_eq "collapse-only is deterministic" "$FOLDED" "$FOLDED2"
SORTED="$(printf '%s\n' "$FOLDED" | LC_ALL=C sort)"
expect_eq "collapse-only output is sorted" "$SORTED" "$FOLDED"

# --- SVG rendering ----------------------------------------------------------

SVG="$(make_perf_script | python3 "$FG" --title "T" --subtitle "S")"
expect_contains "svg has XML declaration" '<?xml version="1.0"' "$SVG"
expect_contains "svg closes the root element" '</svg>' "$SVG"
expect_contains "svg carries the title" '>T</text>' "$SVG"
expect_contains "svg renders the add_limit frame" 'add_limit' "$SVG"
expect_contains "svg renders rect frames" 'class="frame"' "$SVG"

# Deterministic: byte-identical across two renders of the same input.
SVG2="$(make_perf_script | python3 "$FG" --title "T" --subtitle "S")"
expect_eq "svg render is deterministic" "$SVG" "$SVG2"

# XML metacharacters in frame names are escaped, not emitted raw.
ESC_SVG="$(printf 'bench;a<b>&c 3\n' | python3 "$FG" --from-collapsed)"
expect_contains "frame names are XML-escaped" '&lt;b&gt;&amp;c' "$ESC_SVG"
expect_not_contains "raw unescaped angle bracket is not emitted in a frame title" '<title>a<b>' "$ESC_SVG"

# --- Empty input ------------------------------------------------------------

EMPTY_COLLAPSE="$(printf '' | python3 "$FG" --collapse-only)"
expect_eq "empty input yields empty collapse" "" "$EMPTY_COLLAPSE"

printf '' | python3 "$FG" >/dev/null 2>&1
rc=$?
expect_eq "empty input fails SVG render with exit 1" "1" "$rc"

# --- Summary ----------------------------------------------------------------

printf '\nResults: %d passed, %d failed\n' "$PASS" "$FAIL"
[[ "$FAIL" -eq 0 ]]
