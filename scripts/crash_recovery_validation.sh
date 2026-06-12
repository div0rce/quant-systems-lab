#!/usr/bin/env bash
# M45: SIGKILL crash / torn-tail recovery validation for the append-only event log.
#
# Each trial starts `qsl-replay append-loop` in a durability mode, kills it with SIGKILL
# mid-stream, and checks the recovery contract on the surviving file:
#   - flush/fsync modes: every acknowledged append survives; at most one unacknowledged
#     in-flight record appears (recovered in [acked, acked+1]);
#   - any provably torn tail repairs to a clean log, and the repaired log accepts appends;
#   - ambiguous full-header truncation is classified corrupt and refused, not auto-repaired;
#   - buffered mode is a demonstration only: acknowledged data sitting in the user-space
#     stdio buffer is expected to be lost, and the loss is reported, not asserted against.
#
# Process kill validates crash-mid-append recovery. It does NOT validate power-loss or
# OS-crash durability: the page cache survives SIGKILL, so fsync ordering is exercised but
# its stable-storage guarantee is not falsifiable by this harness.
set -euo pipefail

cd "$(dirname "$0")/.."
source scripts/qsl_common.sh

BIN="${QSL_CRASH_BIN:-build/dev/qsl-replay}"
OUT="${QSL_CRASH_OUT:-results/crash_recovery_validation.txt}"
TRIALS="${QSL_CRASH_TRIALS:-3}"
ACK_BASE="${QSL_CRASH_ACK_BASE:-25}"
ACK_STEP="${QSL_CRASH_ACK_STEP:-25}"
PROVENANCE_SCOPE="crash-recovery-validation"
PROVENANCE_INPUTS=(
    include/qsl/replay/event_log.hpp
    src/replay/event_log.cpp
    apps/qsl-replay
    scripts/crash_recovery_validation.sh
    scripts/qsl_common.sh
    CMakeLists.txt
    CMakePresets.json
    Makefile
)

case "$TRIALS$ACK_BASE$ACK_STEP" in *[!0-9]*)
    echo "error: QSL_CRASH_TRIALS / QSL_CRASH_ACK_BASE / QSL_CRASH_ACK_STEP must be unsigned integers." >&2
    exit 2
    ;;
esac
if [[ "$TRIALS" -lt 1 || "$ACK_BASE" -lt 1 ]]; then
    echo "error: QSL_CRASH_TRIALS and QSL_CRASH_ACK_BASE must be >= 1." >&2
    exit 2
fi
if [[ ! -x "$BIN" ]]; then
    echo "error: $BIN not found; build it first (make crash-recovery)." >&2
    exit 1
fi

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/qsl_crash.XXXXXX")"
WRITER_PID=""
cleanup() {
    [[ -n "$WRITER_PID" ]] && kill -KILL "$WRITER_PID" 2>/dev/null || true
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

fail() {
    echo "error: $*" >&2
    exit 1
}

# Wait until the writer has acknowledged at least $2 appends (complete `ack` lines in $1).
wait_for_acks() {
    local ack_file="$1" want="$2" pid="$3" i count
    for ((i = 0; i < 600; i++)); do
        count="$(grep -c '^ack ' "$ack_file" 2>/dev/null || true)"
        [[ "${count:-0}" -ge "$want" ]] && return 0
        kill -0 "$pid" 2>/dev/null || return 1
        sleep 0.05
    done
    return 1
}

recover_field() {
    local output="$1" field="$2" value
    value="$(awk -v f="$field:" '$1 == f { print $2; exit }' <<<"$output")"
    [[ -n "$value" ]] || fail "recover output is missing '$field:'"
    printf '%s\n' "$value"
}

TRIAL_LINES=()
BUFFERED_LOST=0
TORN_REPAIRED=0
CORRUPT_TAILS=0

# One kill trial: start the writer, kill it once `target` appends are acknowledged, then
# validate recovery, repair, and post-repair appendability.
run_trial() {
    local mode="$1" trial="$2" target="$3"
    local log="$WORK_DIR/${mode}_${trial}.bin" ack_file="$WORK_DIR/${mode}_${trial}.acks"
    local acked recovered tail_state repaired="no" recover_out post_out post_records post_append="skipped"

    "$BIN" append-loop "$log" "$mode" >"$ack_file" &
    WRITER_PID=$!
    wait_for_acks "$ack_file" "$target" "$WRITER_PID" ||
        fail "$mode trial $trial: writer died or stalled before $target acknowledged appends"
    kill -KILL "$WRITER_PID" 2>/dev/null || true
    wait "$WRITER_PID" 2>/dev/null || true
    WRITER_PID=""

    acked="$(grep -c '^ack ' "$ack_file" || true)"
    recover_out="$("$BIN" recover "$log" || true)"
    recovered="$(recover_field "$recover_out" records)"
    tail_state="$(recover_field "$recover_out" tail)"

    case "$tail_state" in
    clean) ;;
    torn)
        "$BIN" recover "$log" --repair >/dev/null || fail "$mode trial $trial: torn-tail repair failed"
        recover_out="$("$BIN" recover "$log")" || fail "$mode trial $trial: log not clean after repair"
        [[ "$(recover_field "$recover_out" records)" == "$recovered" ]] ||
            fail "$mode trial $trial: repair changed the recovered record count"
        repaired="yes"
        TORN_REPAIRED=$((TORN_REPAIRED + 1))
        ;;
    corrupt)
        CORRUPT_TAILS=$((CORRUPT_TAILS + 1))
        ;;
    *) fail "$mode trial $trial: unexpected tail state '$tail_state' (acked=$acked)" ;;
    esac

    if [[ "$mode" == "buffered" ]]; then
        [[ "$recovered" -le "$acked" ]] ||
            fail "buffered trial $trial: recovered $recovered exceeds acknowledged $acked"
        BUFFERED_LOST=$((acked - recovered))
    else
        [[ "$recovered" -ge "$acked" && "$recovered" -le $((acked + 1)) ]] ||
            fail "$mode trial $trial: recovered $recovered outside [acked, acked+1] = [$acked, $((acked + 1))]"
    fi

    if [[ "$tail_state" != "corrupt" ]]; then
        post_out="$("$BIN" append-loop "$log" "$mode" 3)" || fail "$mode trial $trial: post-repair append failed"
        recover_out="$("$BIN" recover "$log")" || fail "$mode trial $trial: log not clean after post-repair append"
        post_records="$(recover_field "$recover_out" records)"
        [[ "$post_records" -eq $((recovered + 3)) ]] ||
            fail "$mode trial $trial: post-repair append count $post_records != $((recovered + 3))"
        post_append="ok"
    fi

    TRIAL_LINES+=("$(printf '%-8s trial=%d target_acks=%-4d acked=%-5d recovered=%-5d tail=%-7s repaired=%-3s post_append=%s' \
        "$mode" "$trial" "$target" "$acked" "$recovered" "$tail_state" "$repaired" "$post_append")")
}

for mode in fsync flush; do
    for ((trial = 1; trial <= TRIALS; trial++)); do
        run_trial "$mode" "$trial" $((ACK_BASE + (trial - 1) * ACK_STEP))
    done
done
# Buffered demonstration: kill only after enough acks that the unflushed stdio tail is visible.
run_trial buffered 1 1000

TMP_OUT="$WORK_DIR/artifact.txt"
{
    echo "Command:        make crash-recovery"
    echo "Evidence class: crash-validation (process-kill only; no power-loss claim)"
    echo "Hardware:       $(uname -m)"
    echo "CPU:            $(qsl_cpu_model)"
    echo "OS:             $(uname -s) $(uname -r)"
    echo "Compiler:       $(qsl_build_compiler_version build/dev)"
    echo "Build type:     $(qsl_build_type build/dev)"
    qsl_emit_provenance "$PROVENANCE_SCOPE" "$OUT" "${PROVENANCE_INPUTS[@]}"
    echo "Dataset:        deterministic 16-byte-payload records from qsl-replay append-loop"
    echo "Scenario:       SIGKILL the writer mid-stream per durability mode, then recover/repair"
    echo
    echo "Contract checked per trial:"
    echo "  - flush/fsync: recovered records in [acked, acked+1] (no acknowledged append lost,"
    echo "    at most one unacknowledged in-flight record present);"
    echo "  - partial-header torn tails repair to a clean log without changing recovered count;"
    echo "  - full-header ambiguous truncation classifies as corrupt and is not auto-repaired;"
    echo "  - clean and repaired logs accept appends and read back clean."
    echo
    echo "Metric / Result:"
    for line in "${TRIAL_LINES[@]}"; do
        echo "  $line"
    done
    echo
    echo "Result: all $TRIALS fsync and $TRIALS flush process-kill trials preserved every"
    echo "acknowledged record. Torn tails repaired to clean appendable logs when provable."
    echo "Result: repaired torn tails=$TORN_REPAIRED; corrupt ambiguous tails refused=$CORRUPT_TAILS."
    echo "Result: the buffered demonstration lost $BUFFERED_LOST acknowledged record(s) under"
    echo "SIGKILL; user-space buffering is expected to lose its unflushed tail."
    echo
    echo "Caveat: SIGKILL leaves the kernel page cache intact, so these trials validate"
    echo "crash-mid-append recovery and process-death retention only. They do not validate"
    echo "power-loss or OS-crash durability; the fsync mode's stable-storage guarantee is"
    echo "exercised but not falsifiable by this harness (see docs/persistence.md)."
} >"$TMP_OUT"

mkdir -p "$(dirname "$OUT")"
mv "$TMP_OUT" "$OUT"
echo "wrote $OUT"
cat "$OUT"
