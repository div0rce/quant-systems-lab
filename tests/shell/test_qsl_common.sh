#!/usr/bin/env bash
# Unit tests for scripts/qsl_common.sh::qsl_publish_artifact.
#
# qsl_publish_artifact is the single publish point every artifact generator uses
# instead of a bare `mv`. It must:
#   1. Sanitize host MAC identifiers: redact every MAC-shaped token except the
#      universal broadcast address ff:ff:ff:ff:ff:ff, plus the colon-free
#      wlx<mac> altname, so committed evidence never leaks stable hardware IDs.
#   2. Trim trailing horizontal whitespace on every line.
#   3. Strip trailing blank lines at EOF.
#   4. Reject missing/empty path arguments (exit 2).
#
# The MAC-sanitization cases mirror the limitations-audit leak regex so any
# address that regex would flag is proven redacted before publish. This test is
# registered with CTest (see tests/CMakeLists.txt) and runs under `make check`.
#
# Run directly: bash tests/shell/test_qsl_common.sh
# Exit status: 0 if all tests pass, non-zero otherwise.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Set QSL_REPO_ROOT so sourcing does not shell out to git at source time.
export QSL_REPO_ROOT="$REPO_ROOT"
# shellcheck source=../../scripts/qsl_common.sh disable=SC1091
source "$REPO_ROOT/scripts/qsl_common.sh"

PASS=0
FAIL=0

# Compare actual vs expected exact string; report PASS/FAIL.
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

# Assert a numeric exit code is non-zero (the publish was refused).
expect_nonzero() {
    local name="$1" actual="$2"
    if [[ "$actual" -ne 0 ]]; then
        printf 'PASS: %s (exit %s)\n' "$name" "$actual"
        PASS=$((PASS + 1))
    else
        printf 'FAIL: %s expected non-zero, got 0\n' "$name"
        FAIL=$((FAIL + 1))
    fi
}

# Assert a string contains no non-broadcast MAC-shaped token (the audit grep).
expect_no_leaked_mac() {
    local name="$1" content="$2" hit
    hit="$(printf '%s\n' "$content" |
        grep -nE '([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}' | grep -v 'ff:ff:ff:ff:ff:ff' || true)"
    if [[ -z "$hit" ]]; then
        printf 'PASS: %s\n' "$name"
        PASS=$((PASS + 1))
    else
        printf 'FAIL: %s\n      leaked: %s\n' "$name" "$hit"
        FAIL=$((FAIL + 1))
    fi
}

# Run qsl_publish_artifact on the given input; echo the published content.
publish() {
    local input="$1" tmp out
    tmp="$(mktemp)"
    out="$(mktemp)"
    printf '%s' "$input" >"$tmp"
    qsl_publish_artifact "$tmp" "$out"
    cat "$out"
    rm -f "$tmp" "$out"
}

# --- Trailing-whitespace and blank-line trimming ----------------------------

expect_eq "clean content is preserved" \
    $'alpha\nbeta' "$(publish $'alpha\nbeta\n')"

expect_eq "trailing spaces are removed" \
    $'alpha\nbeta' "$(publish $'alpha   \nbeta\t \n')"

expect_eq "trailing blank lines at EOF are stripped" \
    $'alpha\nbeta' "$(publish $'alpha\nbeta\n\n\n')"

expect_eq "internal blank lines are preserved" \
    $'alpha\n\nbeta' "$(publish $'alpha\n\nbeta\n')"

expect_eq "whitespace-only middle line becomes empty but is preserved" \
    $'alpha\n\nbeta' "$(publish $'alpha\n   \nbeta\n')"

expect_eq "a file of only blank lines becomes empty" \
    "" "$(publish $'\n  \n\t\n')"

# --- MAC sanitization (the security-relevant behavior) ----------------------

expect_eq "link/ether MAC is redacted, broadcast preserved" \
    'link/ether xx:xx:xx:xx:xx:xx brd ff:ff:ff:ff:ff:ff' \
    "$(publish $'link/ether 72:6c:48:ac:17:36 brd ff:ff:ff:ff:ff:ff\n')"

expect_eq "permaddr MAC is redacted" \
    'link/ether xx:xx:xx:xx:xx:xx permaddr xx:xx:xx:xx:xx:xx' \
    "$(publish $'link/ether 0a:1b:2c:3d:4e:5f permaddr 0a:1b:2c:3d:4e:60\n')"

expect_eq "bridge_id and designated_root MACs are redacted" \
    'bridge_id 8000.xx:xx:xx:xx:xx:xx designated_root 8000.xx:xx:xx:xx:xx:xx' \
    "$(publish $'bridge_id 8000.72:6c:48:ac:17:36 designated_root 8000.72:6c:48:ac:17:36\n')"

expect_eq "group_address MAC is redacted" \
    'group_address xx:xx:xx:xx:xx:xx' \
    "$(publish $'group_address 01:80:c2:00:00:00\n')"

expect_eq "wlx<mac> altname is redacted" \
    'altname wlxxxxxxxxxxxxx' \
    "$(publish $'altname wlx726c48ac1736\n')"

# Composite: a realistic ip -details block must publish with no leaked MAC.
ip_details=$'2: wld0: <BROADCAST,MULTICAST,UP> mtu 1500 state UP\n    link/ether 72:6c:48:ac:17:36 brd ff:ff:ff:ff:ff:ff permaddr 72:6c:48:ac:17:37\n    altname wlx726c48ac1736\n4: br0: <BROADCAST> bridge_id 8000.aa:bb:cc:dd:ee:ff designated_root 8000.aa:bb:cc:dd:ee:ff group_address 01:80:c2:00:00:00\n'
expect_no_leaked_mac "composite ip -details block leaks no non-broadcast MAC" "$(publish "$ip_details")"
expect_eq "composite block keeps the broadcast address" \
    'yes' \
    "$(publish "$ip_details" | grep -q 'brd ff:ff:ff:ff:ff:ff' && echo yes || echo no)"

# --- Argument validation ----------------------------------------------------

# Unset args trip nounset inside the function, so it refuses with a non-zero
# code; set-but-empty args reach the explicit guard and exit 2.
( qsl_publish_artifact >/dev/null 2>&1 ); rc=$?
expect_nonzero "missing both arguments is refused" "$rc"

( qsl_publish_artifact /tmp/x >/dev/null 2>&1 ); rc=$?
expect_nonzero "missing second argument is refused" "$rc"

( qsl_publish_artifact "" "" >/dev/null 2>&1 ); rc=$?
expect_eq "empty arguments exit 2" "2" "$rc"

# --- Summary ----------------------------------------------------------------

printf '\nResults: %d passed, %d failed\n' "$PASS" "$FAIL"
[[ "$FAIL" -eq 0 ]]
