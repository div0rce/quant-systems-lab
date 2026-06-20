#!/usr/bin/env bash
# Record DPDK environment support without binding devices, reserving hugepages, or running packet I/O.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=scripts/qsl_common.sh
source scripts/qsl_common.sh

OUT="${QSL_DPDK_ENV_OUT:-results/dpdk_environment.txt}"
PROVENANCE_SCOPE="dpdk-environment-check"
PROVENANCE_INPUTS=(
    Makefile
    CLAUDE.md
    AGENTS.md
    MILESTONES.md
    README.md
    docs/dpdk_research.md
    docs/linux_performance.md
    docs/socket_profiling.md
    results/README.md
    scripts/dpdk_environment_check.sh
    scripts/qsl_common.sh
)

shell_quote() {
    printf '%q' "$1"
}

recorded_command() {
    if [[ -n "${QSL_DPDK_ENV_OUT+x}" ]]; then
        printf 'QSL_DPDK_ENV_OUT=%s ' "$(shell_quote "$OUT")"
    fi
    printf 'make dpdk-check\n'
}

command_path_or_none() {
    local cmd="$1" path
    path="$(command -v "$cmd" 2>/dev/null || true)"
    if [[ -n "$path" ]]; then printf '%s\n' "$path"; else printf 'not-found\n'; fi
}

first_existing_command() {
    local cmd path
    for cmd in "$@"; do
        path="$(command -v "$cmd" 2>/dev/null || true)"
        if [[ -n "$path" ]]; then
            printf '%s\n' "$path"
            return 0
        fi
    done
    printf 'not-found\n'
}

meminfo_field() {
    local key="$1"
    awk -v key="$key" '$1 == key ":" { print $2; exit }' /proc/meminfo 2>/dev/null || true
}

hugepage_mount_state() {
    if [[ ! -d /dev/hugepages ]]; then
        printf 'missing\n'
        return
    fi
    if command -v findmnt >/dev/null 2>&1 && findmnt -n /dev/hugepages >/dev/null 2>&1; then
        printf 'mounted\n'
        return
    fi
    if mount 2>/dev/null | grep -q ' on /dev/hugepages '; then
        printf 'mounted\n'
        return
    fi
    printf 'directory-only\n'
}

module_loaded() {
    local module="$1"
    if [[ -r /proc/modules ]] && grep -q "^${module}[[:space:]]" /proc/modules; then
        printf 'yes\n'
    else
        printf 'no\n'
    fi
}

nonempty_iommu_groups() {
    if [[ -d /sys/kernel/iommu_groups ]] && find /sys/kernel/iommu_groups -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null | grep -q .; then
        printf 'yes\n'
    else
        printf 'no\n'
    fi
}

positive_integer() {
    local value="$1"
    [[ "$value" =~ ^[0-9]+$ && "$value" != "0" ]]
}

dpdk_pkg_version() {
    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libdpdk; then
        pkg-config --modversion libdpdk
    else
        printf 'not-found\n'
    fi
}

dpdk_pkg_flags_status() {
    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libdpdk; then
        printf 'available\n'
    else
        printf 'not-available\n'
    fi
}

write_devbind_status() {
    local devbind="$1"
    if [[ "$devbind" == "not-found" ]]; then
        echo "dpdk-devbind status: not collected (tool not found)"
        return
    fi
    echo
    echo "== dpdk-devbind --status =="
    "$devbind" --status 2>&1 || true
}

mkdir -p "$(dirname "$OUT")"
TMP_OUT="$(mktemp)"
DEVBIND_STATUS="$(mktemp)"
trap 'rm -f "$TMP_OUT" "$DEVBIND_STATUS"' EXIT

OS_NAME="$(uname -s)"
COMMAND_LINE="$(recorded_command)"
PKG_CONFIG_PATH="$(command_path_or_none pkg-config)"
MESON_PATH="$(command_path_or_none meson)"
NINJA_PATH="$(command_path_or_none ninja)"
CC_PATH="$(first_existing_command cc gcc clang)"
CXX_PATH="$(first_existing_command c++ g++ clang++)"
DPDK_DEVBIND_PATH="$(first_existing_command dpdk-devbind dpdk-devbind.py)"
DPDK_HUGEPAGES_PATH="$(first_existing_command dpdk-hugepages dpdk-hugepages.py)"
DPDK_VERSION="$(dpdk_pkg_version)"
DPDK_PKG_STATUS="$(dpdk_pkg_flags_status)"

EVIDENCE_CLASS="unsupported-host"
HOST_SUPPORT_SUMMARY="unsupported OS for DPDK packet-path prototype"
HUGEPAGES_TOTAL="n/a"
HUGEPAGES_FREE="n/a"
HUGEPAGE_SIZE_KB="n/a"
HUGEPAGE_MOUNT="n/a"
VFIO_PCI_LOADED="n/a"
UIO_LOADED="n/a"
IOMMU_GROUPS="n/a"
LSPCI_PATH="not-found"

if [[ "$OS_NAME" == "Linux" ]]; then
    HUGEPAGES_TOTAL="$(meminfo_field HugePages_Total)"
    HUGEPAGES_FREE="$(meminfo_field HugePages_Free)"
    HUGEPAGE_SIZE_KB="$(meminfo_field Hugepagesize)"
    [[ -z "$HUGEPAGES_TOTAL" ]] && HUGEPAGES_TOTAL="unknown"
    [[ -z "$HUGEPAGES_FREE" ]] && HUGEPAGES_FREE="unknown"
    [[ -z "$HUGEPAGE_SIZE_KB" ]] && HUGEPAGE_SIZE_KB="unknown"
    HUGEPAGE_MOUNT="$(hugepage_mount_state)"
    VFIO_PCI_LOADED="$(module_loaded vfio_pci)"
    UIO_LOADED="$(module_loaded uio)"
    IOMMU_GROUPS="$(nonempty_iommu_groups)"
    LSPCI_PATH="$(command_path_or_none lspci)"

    if [[ "$DPDK_PKG_STATUS" != "available" ]]; then
        EVIDENCE_CLASS="linux-missing-dpdk"
        HOST_SUPPORT_SUMMARY="Linux host, but libdpdk is not visible through pkg-config"
    elif [[ "$HUGEPAGE_MOUNT" != "mounted" ]] ||
        ! positive_integer "$HUGEPAGES_TOTAL" ||
        ! positive_integer "$HUGEPAGES_FREE"; then
        EVIDENCE_CLASS="linux-dpdk-constrained"
        HOST_SUPPORT_SUMMARY="DPDK development files are visible, but usable hugepage support is not ready"
    elif [[ "$DPDK_DEVBIND_PATH" == "not-found" ]]; then
        EVIDENCE_CLASS="linux-dpdk-constrained"
        HOST_SUPPORT_SUMMARY="DPDK build files and hugepages are visible, but devbind tooling is missing"
    else
        EVIDENCE_CLASS="linux-dpdk-build-ready"
        HOST_SUPPORT_SUMMARY="DPDK development files, hugepages, and devbind tooling are visible; packet-path measurement still requires an explicitly bound device"
    fi
fi

if [[ "$OS_NAME" == "Linux" && "$DPDK_DEVBIND_PATH" != "not-found" ]]; then
    "$DPDK_DEVBIND_PATH" --status >"$DEVBIND_STATUS" 2>&1 || true
fi

{
    echo "Command:     $COMMAND_LINE"
    echo "Artifact:    DPDK environment support check (non-mutating)"
    echo "Evidence class: $EVIDENCE_CLASS"
    echo "Host support summary: $HOST_SUPPORT_SUMMARY"
    echo "Hardware:    $(uname -m)"
    echo "OS:          $(uname -s) $(uname -r)"
    echo "CPU:         $(qsl_cpu_model)"
    echo "Compiler:    $(qsl_compiler_version)"
    echo "Build type:  n/a"
    qsl_emit_provenance "$PROVENANCE_SCOPE" "$OUT" "${PROVENANCE_INPUTS[@]}"
    echo "pkg-config:  $PKG_CONFIG_PATH"
    echo "libdpdk pkg-config status: $DPDK_PKG_STATUS"
    echo "libdpdk version: $DPDK_VERSION"
    echo "meson:       $MESON_PATH"
    echo "ninja:       $NINJA_PATH"
    echo "cc:          $CC_PATH"
    echo "c++:         $CXX_PATH"
    echo "dpdk-devbind: $DPDK_DEVBIND_PATH"
    echo "dpdk-hugepages: $DPDK_HUGEPAGES_PATH"
    echo "lspci:       $LSPCI_PATH"
    echo "HugePages_Total: $HUGEPAGES_TOTAL"
    echo "HugePages_Free:  $HUGEPAGES_FREE"
    echo "Hugepagesize_kB: $HUGEPAGE_SIZE_KB"
    echo "Hugepage mount /dev/hugepages: $HUGEPAGE_MOUNT"
    echo "vfio-pci loaded: $VFIO_PCI_LOADED"
    echo "uio loaded:      $UIO_LOADED"
    echo "IOMMU groups present: $IOMMU_GROUPS"
    echo "Device binding performed: no"
    echo "Hugepage setup performed: no"
    echo "Packet-path benchmark ran: no"
    echo "Prototype compiled: no"
    echo
    echo "Caveat: This artifact only records whether the host appears capable of building or"
    echo "running a DPDK prototype. It does not bind NICs, reserve hugepages, send packets,"
    echo "or support any kernel-bypass latency/throughput claim."
    if [[ -s "$DEVBIND_STATUS" ]]; then
        echo
        echo "== dpdk-devbind --status =="
        cat "$DEVBIND_STATUS"
    elif [[ "$OS_NAME" != "Linux" ]]; then
        echo "dpdk-devbind status: not collected (unsupported OS)"
    else
        write_devbind_status "$DPDK_DEVBIND_PATH"
    fi
} >"$TMP_OUT"

qsl_publish_artifact "$TMP_OUT" "$OUT"
echo "wrote $OUT"
cat "$OUT"
