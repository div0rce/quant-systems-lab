#!/usr/bin/env bash
# Record NIC offload and timestamping capability context without changing device state.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=scripts/qsl_common.sh
source scripts/qsl_common.sh

OUT="${QSL_NIC_OFFLOAD_OUT:-results/nic_offload_environment.txt}"
PROVENANCE_SCOPE="nic-offload-environment-check"
PROVENANCE_INPUTS=(
    Makefile
    CLAUDE.md
    AGENTS.md
    MILESTONES.md
    README.md
    docs/dpdk_research.md
    docs/linux_performance.md
    docs/nic_offload_study.md
    docs/socket_profiling.md
    results/README.md
    scripts/nic_offload_check.sh
    scripts/qsl_common.sh
)

shell_quote() {
    printf '%q' "$1"
}

recorded_command() {
    if [[ -n "${QSL_NIC_OFFLOAD_OUT+x}" ]]; then
        printf 'QSL_NIC_OFFLOAD_OUT=%s ' "$(shell_quote "$OUT")"
    fi
    if [[ -n "${QSL_NIC_DEVICES:-}" ]]; then
        printf 'QSL_NIC_DEVICES=%s ' "$(shell_quote "$QSL_NIC_DEVICES")"
    fi
    printf 'make nic-offload-check\n'
}

command_path_or_none() {
    local cmd="$1" path
    path="$(command -v "$cmd" 2>/dev/null || true)"
    if [[ -n "$path" ]]; then printf '%s\n' "$path"; else printf 'not-found\n'; fi
}

linux_net_devices() {
    local path dev
    shopt -s nullglob
    for path in /sys/class/net/*; do
        dev="$(basename "$path")"
        [[ "$dev" == "lo" ]] && continue
        printf '%s\n' "$dev"
    done | LC_ALL=C sort
    shopt -u nullglob
}

requested_devices() {
    local devices=()
    if [[ -n "${QSL_NIC_DEVICES:-}" ]]; then
        # Whitespace-separated device list, for example: QSL_NIC_DEVICES="eth0 ens5f0".
        read -r -a devices <<<"$QSL_NIC_DEVICES"
        printf '%s\n' "${devices[@]}"
    else
        linux_net_devices
    fi
}

device_pci_address() {
    local dev="$1" device_path
    device_path="$(readlink -f "/sys/class/net/$dev/device" 2>/dev/null || true)"
    if [[ -n "$device_path" && "$(basename "$device_path")" =~ ^[0-9a-fA-F:.]+$ ]]; then
        basename "$device_path"
    else
        printf 'n/a\n'
    fi
}

device_driver_name() {
    local dev="$1" driver_path
    driver_path="$(readlink -f "/sys/class/net/$dev/device/driver" 2>/dev/null || true)"
    if [[ -n "$driver_path" ]]; then
        basename "$driver_path"
    else
        printf 'n/a\n'
    fi
}

queue_count() {
    local pattern="$1"
    local count=0 match
    while IFS= read -r match; do
        [[ -n "$match" ]] || continue
        ((count += 1))
    done < <(compgen -G "$pattern" || true)
    printf '%s\n' "$count"
}

cat_or_na() {
    local file="$1"
    if [[ -r "$file" ]]; then
        tr -d '\n' <"$file"
    else
        printf 'n/a'
    fi
}

run_section() {
    local title="$1"
    shift
    echo
    echo "== $title =="
    if "$@" 2>&1; then
        return 0
    fi
    echo "command failed: $*"
    return 1
}

mkdir -p "$(dirname "$OUT")"
TMP_OUT="$(mktemp)"
PROBE_DIR="$(mktemp -d)"
trap 'rm -f "$TMP_OUT"; rm -rf "$PROBE_DIR"' EXIT

OS_NAME="$(uname -s)"
COMMAND_LINE="$(recorded_command)"
ETHTOOL_PATH="$(command_path_or_none ethtool)"
IP_PATH="$(command_path_or_none ip)"
LSPCI_PATH="$(command_path_or_none lspci)"
PHC_CTL_PATH="$(command_path_or_none phc_ctl)"
PTP4L_PATH="$(command_path_or_none ptp4l)"
EVIDENCE_CLASS="unsupported-host"
HOST_SUPPORT_SUMMARY="unsupported OS for NIC offload/timestamping inspection"
DEVICE_COUNT="n/a"
REQUESTED_DEVICE_LIST="n/a"
MISSING_DEVICE_LIST="n/a"
OFFLOAD_LIST_VISIBLE="n/a"
RSS_VISIBLE="n/a"
CHANNELS_VISIBLE="n/a"
HARDWARE_TIMESTAMPING_VISIBLE="n/a"
LINUX_DEVICE_LIST="n/a"

if [[ "$OS_NAME" == "Linux" ]]; then
    mapfile -t REQUESTED_DEVICES < <(requested_devices)
    REQUESTED_DEVICE_LIST="${REQUESTED_DEVICES[*]:-none}"
    DEVICES=()
    MISSING_DEVICES=()
    for dev in "${REQUESTED_DEVICES[@]}"; do
        if [[ -e "/sys/class/net/$dev" ]]; then
            DEVICES+=("$dev")
        else
            MISSING_DEVICES+=("$dev")
        fi
    done
    DEVICE_COUNT="${#DEVICES[@]}"
    MISSING_DEVICE_LIST="${MISSING_DEVICES[*]:-none}"
    if [[ "$DEVICE_COUNT" -eq 0 ]]; then
        EVIDENCE_CLASS="linux-no-observable-nic"
        HOST_SUPPORT_SUMMARY="Linux host, but no requested/non-loopback network device was visible"
        OFFLOAD_LIST_VISIBLE="no"
        RSS_VISIBLE="no"
        CHANNELS_VISIBLE="no"
        HARDWARE_TIMESTAMPING_VISIBLE="no"
        LINUX_DEVICE_LIST="none"
    elif [[ "$ETHTOOL_PATH" == "not-found" ]]; then
        EVIDENCE_CLASS="linux-missing-ethtool"
        HOST_SUPPORT_SUMMARY="Linux host and network devices are visible, but ethtool is missing"
        OFFLOAD_LIST_VISIBLE="unknown"
        RSS_VISIBLE="unknown"
        CHANNELS_VISIBLE="unknown"
        HARDWARE_TIMESTAMPING_VISIBLE="unknown"
        LINUX_DEVICE_LIST="${DEVICES[*]}"
    else
        EVIDENCE_CLASS="linux-readonly-capability-observation"
        HOST_SUPPORT_SUMMARY="Linux host with read-only NIC capability inspection; no settings changed and no packet measurement ran"
        OFFLOAD_LIST_VISIBLE="no"
        RSS_VISIBLE="no"
        CHANNELS_VISIBLE="no"
        HARDWARE_TIMESTAMPING_VISIBLE="no"
        LINUX_DEVICE_LIST="${DEVICES[*]}"

        for dev in "${DEVICES[@]}"; do
            [[ -e "/sys/class/net/$dev" ]] || continue
            "$ETHTOOL_PATH" -k "$dev" >"$PROBE_DIR/${dev}.features" 2>&1 &&
                OFFLOAD_LIST_VISIBLE="yes" || true
            "$ETHTOOL_PATH" -x "$dev" >"$PROBE_DIR/${dev}.rss" 2>&1 &&
                RSS_VISIBLE="yes" || true
            "$ETHTOOL_PATH" -l "$dev" >"$PROBE_DIR/${dev}.channels" 2>&1 &&
                CHANNELS_VISIBLE="yes" || true
            if "$ETHTOOL_PATH" -T "$dev" >"$PROBE_DIR/${dev}.timestamping" 2>&1; then
                if grep -Eq 'hardware-(transmit|receive|raw-clock)' "$PROBE_DIR/${dev}.timestamping"; then
                    HARDWARE_TIMESTAMPING_VISIBLE="yes"
                fi
            fi
        done
    fi
fi

{
    echo "Command:     $COMMAND_LINE"
    echo "Artifact:    NIC offload and timestamping capability check (non-mutating)"
    echo "Evidence class: $EVIDENCE_CLASS"
    echo "Host support summary: $HOST_SUPPORT_SUMMARY"
    echo "Hardware:    $(uname -m)"
    echo "OS:          $(uname -s) $(uname -r)"
    echo "CPU:         $(qsl_cpu_model)"
    echo "Compiler:    $(qsl_compiler_version)"
    echo "Build type:  n/a"
    qsl_emit_provenance "$PROVENANCE_SCOPE" "$OUT" "${PROVENANCE_INPUTS[@]}"
    echo "ethtool:     $ETHTOOL_PATH"
    echo "ip:          $IP_PATH"
    echo "lspci:       $LSPCI_PATH"
    echo "phc_ctl:     $PHC_CTL_PATH"
    echo "ptp4l:       $PTP4L_PATH"
    echo "Requested Linux devices: $REQUESTED_DEVICE_LIST"
    echo "Missing requested devices: $MISSING_DEVICE_LIST"
    echo "Linux devices inspected: $LINUX_DEVICE_LIST"
    echo "Device count: $DEVICE_COUNT"
    echo "Offload feature list visible: $OFFLOAD_LIST_VISIBLE"
    echo "RSS indirection/hash visible: $RSS_VISIBLE"
    echo "Queue/channel info visible: $CHANNELS_VISIBLE"
    echo "Hardware timestamping visible: $HARDWARE_TIMESTAMPING_VISIBLE"
    echo "Offload settings changed: no"
    echo "RSS settings changed: no"
    echo "Timestamping configured: no"
    echo "Driver binding changed: no"
    echo "Packet generator ran: no"
    echo "Latency benchmark ran: no"
    echo
    echo "Caveat: This artifact records read-only host and NIC capability context. It does"
    echo "not change offload flags, queue counts, RSS tables, timestamp filters, drivers,"
    echo "or interrupt affinity, and it does not support any NIC-offload or latency claim."

    if [[ "$OS_NAME" == "Linux" && "${DEVICE_COUNT:-0}" != "0" ]]; then
        for dev in "${DEVICES[@]}"; do
            if [[ ! -e "/sys/class/net/$dev" ]]; then
                echo
                echo "== device $dev =="
                echo "device not present under /sys/class/net"
                continue
            fi
            echo
            echo "== device $dev summary =="
            echo "operstate: $(cat_or_na "/sys/class/net/$dev/operstate")"
            echo "mtu:       $(cat_or_na "/sys/class/net/$dev/mtu")"
            echo "driver:    $(device_driver_name "$dev")"
            echo "pci:       $(device_pci_address "$dev")"
            echo "rx queues: $(queue_count "/sys/class/net/$dev/queues/rx-*")"
            echo "tx queues: $(queue_count "/sys/class/net/$dev/queues/tx-*")"
            if [[ "$LSPCI_PATH" != "not-found" && "$(device_pci_address "$dev")" != "n/a" ]]; then
                run_section "lspci -s $(device_pci_address "$dev")" "$LSPCI_PATH" -s "$(device_pci_address "$dev")" || true
            fi
            if [[ "$IP_PATH" != "not-found" ]]; then
                run_section "ip -details link show dev $dev" "$IP_PATH" -details link show dev "$dev" || true
            fi
            if [[ "$ETHTOOL_PATH" != "not-found" ]]; then
                run_section "ethtool -i $dev" "$ETHTOOL_PATH" -i "$dev" || true
                run_section "ethtool -k $dev" "$ETHTOOL_PATH" -k "$dev" || true
                run_section "ethtool -l $dev" "$ETHTOOL_PATH" -l "$dev" || true
                run_section "ethtool -x $dev" "$ETHTOOL_PATH" -x "$dev" || true
                run_section "ethtool -T $dev" "$ETHTOOL_PATH" -T "$dev" || true
            fi
        done
    elif [[ "$OS_NAME" != "Linux" ]]; then
        echo "Device inspection: not collected (unsupported OS)"
    fi
} >"$TMP_OUT"

mv "$TMP_OUT" "$OUT"
echo "wrote $OUT"
cat "$OUT"
