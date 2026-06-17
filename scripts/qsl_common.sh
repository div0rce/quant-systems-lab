#!/usr/bin/env bash
# Shared helpers for QSL shell-based profiling and benchmark artifacts.

qsl_repo_root() {
    local root
    root="$(git rev-parse --show-toplevel)"
    (cd "$root" && pwd -P)
}

QSL_REPO_ROOT="${QSL_REPO_ROOT:-$(qsl_repo_root)}"

qsl_repo_relative_or_empty() {
    local path="$1" abs dir base resolved_dir resolved
    [[ -z "$path" ]] && return
    if [[ "$path" = /* ]]; then abs="$path"; else abs="$QSL_REPO_ROOT/$path"; fi
    dir="$(dirname "$abs")"
    base="$(basename "$abs")"
    resolved_dir="$(cd "$dir" 2>/dev/null && pwd -P)" || return
    resolved="$resolved_dir/$base"
    case "$resolved" in
    "$QSL_REPO_ROOT"/*) printf '%s\n' "${resolved#"$QSL_REPO_ROOT"/}" ;;
    esac
}

qsl_dirty_tree_status() {
    local rel status_output path
    local pathspecs=(.)
    for path in "$@"; do
        rel="$(qsl_repo_relative_or_empty "$path")"
        [[ -n "$rel" ]] && pathspecs+=(":(exclude)$rel")
    done
    if ! status_output="$(git status --porcelain --untracked-files=all -- "${pathspecs[@]}")"; then
        echo "error: dirty-tree check failed; refusing to write misleading metadata." >&2
        exit 2
    fi
    if [[ -n "$status_output" ]]; then echo yes; else echo no; fi
}

qsl_cpu_model() {
    local cpu
    if [[ -r /proc/cpuinfo ]]; then
        cpu="$(grep -m1 -E '^(model name|Processor|cpu model|Hardware)[[:space:]]*:' /proc/cpuinfo 2>/dev/null |
            cut -d: -f2- | sed 's/^ *//' || true)"
    fi
    if [[ -z "${cpu:-}" ]] && command -v lscpu >/dev/null 2>&1; then
        cpu="$(lscpu 2>/dev/null |
            awk -F: '
                /^Architecture:/ { arch = $2; sub(/^[[:space:]]*/, "", arch) }
                /^Vendor ID:/ { vendor = $2; sub(/^[[:space:]]*/, "", vendor) }
                /^Model name:/ { model = $2; sub(/^[[:space:]]*/, "", model) }
                END {
                    if (model != "" && model != "-") print model;
                    else if (vendor != "" && arch != "") print vendor " " arch;
                    else if (arch != "") print arch;
                }
            ' || true)"
    fi
    if [[ -z "${cpu:-}" ]] && [[ "$(uname -s)" == "Darwin" ]]; then
        cpu="$(sysctl -n machdep.cpu.brand_string 2>/dev/null || true)"
    fi
    printf '%s\n' "${cpu:-unknown}"
}

qsl_compiler_version() {
    c++ --version | head -1
}

qsl_build_compiler_version() {
    local build_dir="${1:-build/dev}" compiler resolved
    compiler="$(awk -F= '/^CMAKE_CXX_COMPILER:/ { print $2; exit }' \
        "$build_dir/CMakeCache.txt" 2>/dev/null || true)"
    if [[ -n "$compiler" ]]; then
        if [[ -x "$compiler" ]]; then
            "$compiler" --version | head -1
            return
        fi
        resolved="$(command -v "$compiler" 2>/dev/null || true)"
        if [[ -n "$resolved" ]]; then
            "$resolved" --version | head -1
            return
        fi
    fi
    qsl_compiler_version
}

# Build type (CMAKE_BUILD_TYPE) of the binaries under test. Honors a QSL_BUILD_TYPE override (for
# runs whose build dir is not the default, e.g. containerized regenerations); otherwise reads it
# from the build's CMakeCache. Falls back to "unknown".
qsl_build_type() {
    if [[ -n "${QSL_BUILD_TYPE:-}" ]]; then
        printf '%s\n' "$QSL_BUILD_TYPE"
        return
    fi
    local build_dir="${1:-build/dev}" bt
    bt="$(grep -m1 '^CMAKE_BUILD_TYPE:' "$build_dir/CMakeCache.txt" 2>/dev/null | cut -d= -f2 || true)"
    printf '%s\n' "${bt:-unknown}"
}

qsl_git_commit_short() {
    git rev-parse --short HEAD
}

qsl_sha256_stdin() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum | awk '{ printf "sha256:%s\n", $1 }'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 | awk '{ printf "sha256:%s\n", $1 }'
    else
        echo "error: sha256sum or shasum is required for source digest metadata." >&2
        return 127
    fi
}

qsl_source_digest() {
    local scope="$1" output="$2" output_rel files rel blob
    shift 2
    if [[ -z "$scope" || -z "$output" || "$#" -eq 0 ]]; then
        echo "error: qsl_source_digest requires scope, output, and at least one input path" >&2
        exit 2
    fi

    local pathspecs=("$@")
    output_rel="$(qsl_repo_relative_or_empty "$output")"
    [[ -n "$output_rel" ]] && pathspecs+=(":(exclude)$output_rel")

    if ! files="$(git ls-files -- "${pathspecs[@]}" | LC_ALL=C sort)"; then
        echo "error: source-digest input pathspec failed for scope $scope" >&2
        exit 2
    fi
    if [[ -z "$files" ]]; then
        echo "error: source-digest scope $scope matched no tracked inputs" >&2
        exit 2
    fi

    {
        printf 'scope %s\n' "$scope"
        while IFS= read -r rel; do
            [[ -z "$rel" ]] && continue
            blob="$(git hash-object -- "$rel")"
            printf '%s  %s\n' "$blob" "$rel"
        done <<<"$files"
    } |
        qsl_sha256_stdin
}

qsl_dirty_inputs_status() {
    local output="$1" output_rel status_output
    shift
    if [[ -z "$output" || "$#" -eq 0 ]]; then
        echo "error: qsl_dirty_inputs_status requires output and at least one input path" >&2
        exit 2
    fi

    local pathspecs=("$@")
    output_rel="$(qsl_repo_relative_or_empty "$output")"
    [[ -n "$output_rel" ]] && pathspecs+=(":(exclude)$output_rel")

    if ! status_output="$(git status --porcelain --untracked-files=all -- "${pathspecs[@]}")"; then
        echo "error: dirty-input check failed; refusing to write misleading metadata." >&2
        exit 2
    fi
    if [[ -n "$status_output" ]]; then echo yes; else echo no; fi
}

qsl_emit_provenance() {
    local scope="$1" output="$2" generated_output source_digest dirty_inputs
    shift 2
    if [[ -z "$scope" || -z "$output" || "$#" -eq 0 ]]; then
        echo "error: qsl_emit_provenance requires scope, output, and at least one input path" >&2
        exit 2
    fi

    generated_output="$(qsl_repo_relative_or_empty "$output" || true)"
    [[ -z "$generated_output" ]] && generated_output="$output"
    source_digest="$(qsl_source_digest "$scope" "$output" "$@")"
    dirty_inputs="$(qsl_dirty_inputs_status "$output" "$@")"

    echo "Provenance version: 1"
    echo "Git commit (informational): $(qsl_git_commit_short)"
    echo "Source digest: $source_digest"
    echo "Source digest scope: $scope"
    echo "Dirty inputs: $dirty_inputs"
    echo "Generated output: $generated_output"
    echo "Date: $(qsl_utc_timestamp)"
}

qsl_publish_artifact() {
    local tmp="$1" out="$2" clean
    if [[ -z "$tmp" || -z "$out" ]]; then
        echo "error: qsl_publish_artifact requires temporary and output paths" >&2
        exit 2
    fi
    clean="$(mktemp)"
    sed 's/[[:blank:]]*$//' "$tmp" |
        awk '
            { lines[NR] = $0 }
            END {
                last = NR
                while (last > 0 && lines[last] == "") {
                    --last
                }
                for (i = 1; i <= last; ++i) {
                    print lines[i]
                }
            }
        ' >"$clean"
    mv "$clean" "$out"
}

qsl_utc_timestamp() {
    date -u +%Y-%m-%dT%H:%M:%SZ
}

qsl_require_linux() {
    local script_name="$1" reason="$2"
    if [[ "$(uname -s)" != "Linux" ]]; then
        echo "error: $script_name requires Linux ($reason); current OS is $(uname -s)." >&2
        exit 2
    fi
}

qsl_listen_entry_present() {
    local port="$1" port_hex
    printf -v port_hex '%04X' "$port"
    awk -v want="0100007F:$port_hex" '$2 == want && $4 == "0A" { found = 1 } END { exit !found }' \
        /proc/net/tcp
}

qsl_wait_proc_tcp_listen_ready() {
    local port="$1" pid="$2" attempts="${3:-100}" delay="${4:-0.05}" i
    for ((i = 0; i < attempts; i++)); do
        kill -0 "$pid" 2>/dev/null || return 1
        if qsl_listen_entry_present "$port"; then
            return 0
        fi
        sleep "$delay"
    done
    return 1
}

qsl_wait_tcp_connect_ready() {
    local port="$1" pid="$2" attempts="${3:-100}" delay="${4:-0.05}" i
    for ((i = 0; i < attempts; i++)); do
        kill -0 "$pid" 2>/dev/null || return 1
        if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
            exec 3>&- 3<&-
            return 0
        fi
        sleep "$delay"
    done
    return 1
}

qsl_stop_process_gracefully() {
    local pid="${1:-}" signal="${2:-INT}" attempts="${3:-20}" delay="${4:-0.05}" i
    [[ -z "$pid" ]] && return 0
    kill "-$signal" "$pid" 2>/dev/null || true
    for ((i = 0; i < attempts; i++)); do
        kill -0 "$pid" 2>/dev/null || break
        sleep "$delay"
    done
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}
