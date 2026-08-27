#!/usr/bin/env bash
#
# Builds the macOS installer .pkg for Warehouse from an existing Release build.
# Does not build the plugin itself -- run scripts/build.sh (or the CI configure
# step) first.
#
#   packaging/build-pkg.sh <build-dir> <version> <out-dir>
#
# <build-dir> is a CMake build directory containing
# Warehouse_artefacts/Release/{AU,VST3}, <version> is the plain version string
# (e.g. 1.0.0, no leading "v"), <out-dir> is where the finished .pkg is written.
# Exactly one .pkg lands in <out-dir>; any previous .pkg files there are removed
# first so re-running this script does not leave stale output behind.
#
# Unsigned by design: this script adds no codesign/notarisation step of its
# own. It only copies the AU and VST3 bundles that CMake's own ad-hoc-sign
# POST_BUILD step already produced (see CMakeLists.txt), preserving whatever
# signature is already on disk.
set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "usage: $0 <build-dir> <version> <out-dir>" >&2
    exit 2
fi

BUILD_DIR="$1"
VERSION="$2"
OUT_DIR="$3"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PKG_DIR="$ROOT/packaging/macos"

# Resolve to absolute paths up front: pkgbuild/productbuild are run from a
# scratch working directory below, and relative paths would then resolve
# against the wrong cwd.
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

ARTEFACTS="$BUILD_DIR/Warehouse_artefacts/Release"
AU_SRC="$ARTEFACTS/AU/Warehouse.component"
VST3_SRC="$ARTEFACTS/VST3/Warehouse.vst3"

for src in "$AU_SRC" "$VST3_SRC"; do
    if [[ ! -d "$src" ]]; then
        echo "error: expected build output not found: $src" >&2
        echo "       (build the Warehouse target in $BUILD_DIR first)" >&2
        exit 1
    fi
done

# --- Refuse to package a non-universal build -----------------------------------
#
# This is the guard against the release's worst silent failure. Ableton Live on
# Apple Silicon does not report an error for a VST3 that is not arm64-native --
# it simply omits the plugin from the list, with no diagnostic anywhere. And an
# x86_64-less build is invisible to Intel Macs entirely.
#
# CI always configures with -DUNIVERSAL_BINARY=ON, so CI cannot ship thin. But
# this script is a documented standalone entry point, and `scripts/build.sh`
# with no flags produces a HOST-ARCH-ONLY build. So a release cut by hand --
# `bash scripts/build.sh && packaging/build-pkg.sh build 1.0.0 dist` -- would
# otherwise package an arm64-only bundle without a murmur.
for bin in "$AU_SRC/Contents/MacOS/Warehouse" "$VST3_SRC/Contents/MacOS/Warehouse"; do
    archs="$(lipo -archs "$bin")"

    for required in arm64 x86_64; do
        case " $archs " in
            *" $required "*) ;;
            *)
                echo "error: $bin is missing the $required slice (has: $archs)." >&2
                echo "       A non-universal build must not be released: Ableton Live on Apple" >&2
                echo "       Silicon SILENTLY omits a non-native VST3, and an arm64-only build" >&2
                echo "       does not run on Intel Macs at all." >&2
                echo "       Reconfigure with -DUNIVERSAL_BINARY=ON and rebuild." >&2
                exit 1
                ;;
        esac
    done

    echo "arch check: $(basename "$(dirname "$(dirname "$bin")")") -> $archs"
done

WORK="$(mktemp -d "${TMPDIR:-/tmp}/warehouse-pkg.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# --- Stage one component-package payload root per format --------------------
#
# pkgbuild's --root becomes --install-location verbatim, so each root only
# needs to contain the bundle itself; ditto preserves the extended attributes
# and resource forks that a plain cp/rsync can silently drop, which matters
# here because the bundles are already ad-hoc codesigned.
AU_ROOT="$WORK/au-root"
VST3_ROOT="$WORK/vst3-root"
mkdir -p "$AU_ROOT" "$VST3_ROOT"
ditto "$AU_SRC" "$AU_ROOT/Warehouse.component"
ditto "$VST3_SRC" "$VST3_ROOT/Warehouse.vst3"

pkgbuild \
    --root "$AU_ROOT" \
    --identifier com.emirsezer.warehouse.au \
    --version "$VERSION" \
    --install-location /Library/Audio/Plug-Ins/Components \
    "$WORK/au.pkg"

pkgbuild \
    --root "$VST3_ROOT" \
    --identifier com.emirsezer.warehouse.vst3 \
    --version "$VERSION" \
    --install-location /Library/Audio/Plug-Ins/VST3 \
    "$WORK/vst3.pkg"

# --- Combine into one distribution package with selectable choices ----------
RESOURCES="$WORK/resources"
mkdir -p "$RESOURCES"
cp "$PKG_DIR/resources/welcome.txt" "$RESOURCES/welcome.txt"
cp "$PKG_DIR/resources/readme.txt" "$RESOURCES/readme.txt"
cp "$ROOT/LICENSE" "$RESOURCES/LICENSE.txt"

sed "s|@VERSION@|$VERSION|g" "$PKG_DIR/distribution.xml.in" > "$WORK/distribution.xml"

# Scoped to this product's own filename pattern, not a blanket *.pkg: an
# out-dir that happens to be a directory the caller also uses for other
# installers (e.g. ~/Downloads) must not lose unrelated packages here.
rm -f "$OUT_DIR"/Warehouse-*.pkg

productbuild \
    --distribution "$WORK/distribution.xml" \
    --package-path "$WORK" \
    --resources "$RESOURCES" \
    "$OUT_DIR/Warehouse-$VERSION-macOS.pkg"

echo "wrote $OUT_DIR/Warehouse-$VERSION-macOS.pkg"
