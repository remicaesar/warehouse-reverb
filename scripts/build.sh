#!/usr/bin/env bash
#
# Configures and builds Warehouse.
#
#   scripts/build.sh                  release build, host architecture only
#   scripts/build.sh --debug          debug build
#   scripts/build.sh --universal      arm64 + x86_64 fat binaries
#   scripts/build.sh --target X       build a single target, e.g. WarehouseTests
#   scripts/build.sh --clean          delete the build directory first
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/build"

CONFIG="Release"
UNIVERSAL="OFF"
TARGET=""
CLEAN=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug)      CONFIG="Debug"; shift ;;
        --release)    CONFIG="Release"; shift ;;
        --universal)  UNIVERSAL="ON"; shift ;;
        --target)     TARGET="$2"; shift 2 ;;
        --clean)      CLEAN=1; shift ;;
        -h|--help)    sed -n '2,11p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *)            echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

CMAKE="$(command -v cmake || echo /opt/homebrew/bin/cmake)"

if [[ ! -x "$CMAKE" ]]; then
    echo "cmake not found" >&2
    exit 1
fi

if [[ $CLEAN -eq 1 ]]; then
    rm -rf "$BUILD_DIR"
fi

"$CMAKE" -S "$ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$CONFIG" \
    -DUNIVERSAL_BINARY="$UNIVERSAL"

BUILD_ARGS=("--build" "$BUILD_DIR" "--config" "$CONFIG" "--parallel" "$(sysctl -n hw.ncpu)")

if [[ -n "$TARGET" ]]; then
    BUILD_ARGS+=("--target" "$TARGET")
fi

"$CMAKE" "${BUILD_ARGS[@]}"

echo
echo "Build finished ($CONFIG${TARGET:+, target $TARGET})."
echo "Artefacts under: $BUILD_DIR"
