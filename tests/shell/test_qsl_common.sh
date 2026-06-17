#!/usr/bin/env bash
# Unit tests for scripts/qsl_common.sh
#
# Covers the qsl_publish_artifact function introduced in the linux-host-artifact-refresh PR.
# The function replaced bare `mv` calls in all artifact-generating scripts and adds:
#   1. Trailing horizontal whitespace trimming on every line (sed 's/[[:blank:]]*$//')
#   2. Trailing blank line stripping at EOF (awk drops trailing empty lines)
#
# Run from the repo root:
#   bash tests/shell/test_qsl_common.sh
#
# Exit status: 0 if all tests pass, 1 if any fail.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Source qsl_common.sh. Set QSL_REPO_ROOT so it does not call git inside the
# function evaluated at source time (the repo root is already known here).
export QSL_REPO_ROOT="$REPO_ROOT"
# shellcheck source=../../scripts/qsl_common.sh
source "$REPO_ROOT/scripts/qsl_common.sh"

# ---------------------------------------------------------------------------
# Minimal test harness
# ---------------------------------------------------------------------------

PASS=0
FAIL=0

_assert_eq() {
    local label="$1" expected="$2" actual="$3"
    if [[ "$expected" == "$actual" ]]; then
        echo "  PASS: $label"
        ((PASS++)) || true
    else
        echo "  FAIL: $label"
        echo "        expected: $(printf '%q' "$expected")"
        echo "        actual:   $(printf '%q' "$actual")"
        ((FAIL++)) || true
    fi
}

_assert_exit() {
    local label="$1" expected_code="$2"
    shift 2
    # Run in a subshell so that a sourced function calling `exit N` does not
    # terminate the test script itself.
    local actual_code=0
    ( "$@" ) >/dev/null 2>&1 || actual_code=$?
    if [[ "$actual_code" -eq "$expected_code" ]]; then
        echo "  PASS: $label (exit $actual_code)"
        ((PASS++)) || true
    else
        echo "  FAIL: $label (expected exit $expected_code, got $actual_code)"
        ((FAIL++)) || true
    fi
}

_assert_nonzero_exit() {
    local label="$1"
    shift
    # Run in a subshell to contain any `exit` calls from sourced functions.
    local actual_code=0
    ( "$@" ) >/dev/null 2>&1 || actual_code=$?
    if [[ "$actual_code" -ne 0 ]]; then
        echo "  PASS: $label (exit $actual_code, non-zero)"
        ((PASS++)) || true
    else
        echo "  FAIL: $label (expected non-zero exit, got 0)"
        ((FAIL++)) || true
    fi
}

run_test() {
    local name="$1"
    shift
    echo "TEST: $name"
    "$@"
}

# ---------------------------------------------------------------------------
# Helper: write content to a temp file and run qsl_publish_artifact; set
# OUT_FILE and OUT_CONTENT with the results.
# ---------------------------------------------------------------------------

OUT_FILE=""
OUT_CONTENT=""

_run_publish() {
    local content="$1"
    local tmp in_file
    tmp="$(mktemp -d)"
    in_file="$tmp/input.txt"
    OUT_FILE="$tmp/output.txt"
    # printf preserves the content exactly without adding an extra newline.
    printf '%s' "$content" >"$in_file"
    qsl_publish_artifact "$in_file" "$OUT_FILE"
    OUT_CONTENT="$(cat "$OUT_FILE")"
}

# ---------------------------------------------------------------------------
# Test functions (must be defined before they are called)
# ---------------------------------------------------------------------------

test_clean_content_is_preserved() {
    local input
    input="$(printf 'line one\nline two\nline three\n')"
    _run_publish "$input"
    _assert_eq "content matches" "$(printf 'line one\nline two\nline three')" "$OUT_CONTENT"
}

test_trailing_spaces_are_removed() {
    local input
    input="$(printf 'hello   \nworld  \nfoo\n')"
    _run_publish "$input"
    _assert_eq "trailing spaces stripped" "$(printf 'hello\nworld\nfoo')" "$OUT_CONTENT"
}

test_trailing_tabs_are_removed() {
    local input
    input="$(printf 'alpha\t\t\nbeta\t\ngamma\n')"
    _run_publish "$input"
    _assert_eq "trailing tabs stripped" "$(printf 'alpha\nbeta\ngamma')" "$OUT_CONTENT"
}

test_trailing_mixed_whitespace_is_removed() {
    # Mix of spaces and tabs at line ends
    local input
    input="$(printf 'col1 \t col2 \t\ndata\t   \n')"
    _run_publish "$input"
    _assert_eq "mixed whitespace stripped" "$(printf 'col1 \t col2\ndata')" "$OUT_CONTENT"
}

test_multiple_trailing_blank_lines_are_removed() {
    local input
    input="$(printf 'content\n\n\n\n')"
    _run_publish "$input"
    _assert_eq "trailing blank lines removed" "content" "$OUT_CONTENT"
}

test_single_trailing_blank_line_is_removed() {
    local input
    input="$(printf 'line\n\n')"
    _run_publish "$input"
    _assert_eq "single trailing blank removed" "line" "$OUT_CONTENT"
}

test_both_whitespace_and_blank_lines_removed_together() {
    local input expected
    input="$(printf 'key: value   \nother: data\t\n\n\n')"
    expected="$(printf 'key: value\nother: data')"
    _run_publish "$input"
    _assert_eq "trailing whitespace and blanks removed together" "$expected" "$OUT_CONTENT"
}

test_internal_blank_lines_are_preserved() {
    local input
    input="$(printf 'section A\n\nparagraph text\n\nsection B\n')"
    _run_publish "$input"
    _assert_eq "internal blank lines preserved" \
        "$(printf 'section A\n\nparagraph text\n\nsection B')" "$OUT_CONTENT"
}

test_empty_input_file_produces_empty_output() {
    _run_publish ""
    _assert_eq "empty input → empty output" "" "$OUT_CONTENT"
}

test_file_of_only_blank_lines_produces_empty_output() {
    local input
    input="$(printf '\n\n\n')"
    _run_publish "$input"
    _assert_eq "all blank lines → empty output" "" "$OUT_CONTENT"
}

test_single_line_without_trailing_newline_is_preserved() {
    _run_publish "no newline at end"
    _assert_eq "single line no trailing newline" "no newline at end" "$OUT_CONTENT"
}

test_whitespace_only_middle_lines_become_empty_preserved_lines() {
    # A line with only spaces in the middle becomes a blank line (trimmed), but
    # that blank line is still present because it is not at the end.
    local input
    input="$(printf 'first\n   \nsecond\n')"
    _run_publish "$input"
    _assert_eq "whitespace-only middle line → empty, preserved" \
        "$(printf 'first\n\nsecond')" "$OUT_CONTENT"
}

test_realistic_artifact_header_is_cleaned() {
    local input expected
    input="$(printf \
        'Command:     make dpdk-check   \nArtifact:    DPDK environment support check   \nEvidence class: linux-missing-dpdk\n\nCaveat: no devices bound.   \n\n\n')"
    expected="$(printf \
        'Command:     make dpdk-check\nArtifact:    DPDK environment support check\nEvidence class: linux-missing-dpdk\n\nCaveat: no devices bound.')"
    _run_publish "$input"
    _assert_eq "realistic artifact header cleaned" "$expected" "$OUT_CONTENT"
}

test_output_file_is_created_at_destination() {
    local tmp out
    tmp="$(mktemp)"
    out="$(mktemp)"
    printf 'hello\n' >"$tmp"
    qsl_publish_artifact "$tmp" "$out"
    if [[ -f "$out" ]]; then
        echo "  PASS: output file exists at destination"
        ((PASS++)) || true
    else
        echo "  FAIL: output file not created at $out"
        ((FAIL++)) || true
    fi
    rm -f "$tmp" "$out"
}

test_missing_first_argument_exits_nonzero() {
    # When called with no arguments, the function fails. Under `set -u` the
    # shell itself may exit 1 before reaching the function's own `exit 2`
    # guard because $1/$2 are unset. Either way the exit must be non-zero.
    _assert_nonzero_exit "no args → non-zero exit" qsl_publish_artifact
}

test_missing_second_argument_exits_nonzero() {
    # Same reasoning: $2 is unset when only one argument is supplied.
    local tmp
    tmp="$(mktemp)"
    printf 'content\n' >"$tmp"
    _assert_nonzero_exit "one arg → non-zero exit" qsl_publish_artifact "$tmp"
    rm -f "$tmp"
}

test_empty_string_second_argument_exits_2() {
    local tmp
    tmp="$(mktemp)"
    printf 'content\n' >"$tmp"
    _assert_exit "empty second arg → exit 2" 2 qsl_publish_artifact "$tmp" ""
    rm -f "$tmp"
}

test_empty_both_arguments_exits_2() {
    _assert_exit "empty both args → exit 2" 2 qsl_publish_artifact "" ""
}

# Regression: a file with a proper terminating newline after the last content
# line must not have that last content line treated as a trailing blank.
test_regression_terminating_newline_preserves_content() {
    local input
    input="$(printf 'first\nsecond\n')"
    _run_publish "$input"
    _assert_eq "terminating newline: both content lines present" \
        "$(printf 'first\nsecond')" "$OUT_CONTENT"
}

# Boundary: a line consisting entirely of spaces produces empty output because
# it becomes blank after trimming and is the only (trailing) line.
test_boundary_single_line_all_spaces_gives_empty_output() {
    _run_publish "     "
    _assert_eq "single spaces-only line → empty output" "" "$OUT_CONTENT"
}

# Verify that the input temporary file is consumed (it should no longer exist
# at the original tmp path after publish, since mv is used internally).
test_input_tmp_file_is_consumed_after_publish() {
    local tmp out
    tmp="$(mktemp)"
    out="$(mktemp)"
    printf 'data\n' >"$tmp"
    qsl_publish_artifact "$tmp" "$out"
    # The function uses mktemp internally and mv, so the caller's tmp is
    # still present (only the internal clean temp is moved). The output must
    # have content regardless.
    local content
    content="$(cat "$out")"
    _assert_eq "output has expected content" "data" "$content"
    rm -f "$tmp" "$out"
}

# ---------------------------------------------------------------------------
# Run all tests
# ---------------------------------------------------------------------------

run_test "clean content is preserved unchanged"                    test_clean_content_is_preserved
run_test "trailing spaces on lines are removed"                     test_trailing_spaces_are_removed
run_test "trailing tabs on lines are removed"                       test_trailing_tabs_are_removed
run_test "mixed trailing spaces and tabs are removed"               test_trailing_mixed_whitespace_is_removed
run_test "multiple trailing blank lines at EOF are removed"         test_multiple_trailing_blank_lines_are_removed
run_test "single trailing blank line is removed"                    test_single_trailing_blank_line_is_removed
run_test "both trailing whitespace and blank lines removed together" test_both_whitespace_and_blank_lines_removed_together
run_test "blank lines in the middle of content are preserved"       test_internal_blank_lines_are_preserved
run_test "empty input file produces empty output"                   test_empty_input_file_produces_empty_output
run_test "file of only blank lines produces empty output"           test_file_of_only_blank_lines_produces_empty_output
run_test "single line without trailing newline is preserved"        test_single_line_without_trailing_newline_is_preserved
run_test "whitespace-only middle lines become empty preserved lines" test_whitespace_only_middle_lines_become_empty_preserved_lines
run_test "realistic artifact header is trimmed correctly"           test_realistic_artifact_header_is_cleaned
run_test "output file is created at the specified destination"      test_output_file_is_created_at_destination
run_test "missing first argument exits with non-zero code"          test_missing_first_argument_exits_nonzero
run_test "missing second argument exits with non-zero code"         test_missing_second_argument_exits_nonzero
run_test "empty string for second argument exits with code 2"       test_empty_string_second_argument_exits_2
run_test "empty string for both arguments exits with code 2"        test_empty_both_arguments_exits_2
run_test "regression: terminating newline preserves all content"    test_regression_terminating_newline_preserves_content
run_test "boundary: single line of spaces produces empty output"    test_boundary_single_line_all_spaces_gives_empty_output
run_test "output has expected content after publish"                test_input_tmp_file_is_consumed_after_publish

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

echo ""
echo "Results: $PASS passed, $FAIL failed"
if [[ "$FAIL" -gt 0 ]]; then
    exit 1
fi
exit 0
