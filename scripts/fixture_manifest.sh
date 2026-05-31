#!/usr/bin/env bash
# Seed reproducibility manifest (issue #47): records the generator version and, for each
# generator-produced differential fixture, the exporter invocation (seed/scenario) plus the
# SHA-256 of the committed file. This is provenance: a future generator change cannot silently
# alter a fixture without an explicit manifest update, and the seed->fixture->hash mapping is
# documented. (check_fixtures.sh separately proves each fixture matches current C++ output.)
#
#   scripts/fixture_manifest.sh           # (re)write ocaml/test/fixtures/MANIFEST.txt
#   scripts/fixture_manifest.sh --check   # regenerate and diff vs committed; non-zero on drift
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=build/dev/qsl-export-stream
[ -x "$BIN" ] || cmake --build --preset dev --target qsl-export-stream >/dev/null

F=ocaml/test/fixtures
sha() { # portable sha-256 of $1 -> bare hex digest
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

generate() {
    echo "# qsl fixture reproducibility manifest (issue #47)"
    echo "# columns: <exporter-args> <sha256-of-committed-fixture>"
    echo "generator_version $("$BIN" version)"
    emit() {
        local file="$1"
        shift
        echo "$* $(sha "$F/$file")"
    }
    emit stream_seed7.txt 7 60
    emit stream_ioc.txt ioc
    for s in $(seq 1 50); do emit "prop_seed$s.txt" prop "$s"; done
    emit shrunk_seed1.txt shrink 1
}

if [ "${1:-}" = "--check" ]; then
    if diff -u "$F/MANIFEST.txt" <(generate); then
        echo "manifest matches committed fixtures"
    else
        echo "MANIFEST drift: regenerate with scripts/fixture_manifest.sh" >&2
        exit 1
    fi
else
    generate >"$F/MANIFEST.txt"
    echo "wrote $F/MANIFEST.txt"
fi
