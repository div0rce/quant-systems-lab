#!/usr/bin/env bash
# Cross-compiler / platform determinism (issue #45): build the fixture exporter with two
# compilers and assert every generated differential fixture is byte-identical between them, and
# byte-identical to the committed copies (which were produced on macOS/AppleClang — so this also
# exercises cross-platform reproducibility). Generation is integer-only and uses a standard
# mt19937_64, so the bytes must not depend on the toolchain.
#
# Usage: scripts/determinism_check.sh [cc1 cxx1] [cc2 cxx2]   (defaults: gcc/g++ and clang/clang++)
set -euo pipefail
cd "$(dirname "$0")/.."

CC1="${1:-gcc}"
CXX1="${2:-g++}"
CC2="${3:-clang}"
CXX2="${4:-clang++}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK" build/det' EXIT

emit() { # $1=binary  $2=outdir
    local bin="$1" out="$2"
    mkdir -p "$out"
    "$bin" 7 60 >"$out/stream_seed7.txt"
    "$bin" ioc >"$out/stream_ioc.txt"
    for s in 1 2 3 4 5 6 7 8; do "$bin" prop "$s" >"$out/prop_seed$s.txt"; done
    "$bin" shrink 1 >"$out/shrunk_seed1.txt"
}

build_and_emit() { # $1=cc $2=cxx $3=outdir
    rm -rf build/det
    CC="$1" CXX="$2" cmake -S . -B build/det -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug -DQSL_BUILD_TESTS=OFF >/dev/null
    cmake --build build/det --target qsl-export-stream >/dev/null
    emit build/det/qsl-export-stream "$3"
}

echo "building fixtures with $CXX1 ..." && build_and_emit "$CC1" "$CXX1" "$WORK/a"
echo "building fixtures with $CXX2 ..." && build_and_emit "$CC2" "$CXX2" "$WORK/b"

diff -r "$WORK/a" "$WORK/b"
echo "fixtures byte-identical across $CXX1 and $CXX2"

for f in "$WORK/a"/*; do diff -q "$f" "ocaml/test/fixtures/$(basename "$f")"; done
echo "fixtures byte-identical vs committed (macOS/AppleClang) copies"
