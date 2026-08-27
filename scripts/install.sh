#!/usr/bin/env bash
#
# Copies the built AU and VST3 into the user plug-in folders and ad-hoc signs them in place.
# Idempotent: re-running replaces whatever is already there.
#
#   scripts/install.sh                 install from the Release build
#   scripts/install.sh --debug         install from the Debug build
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="Release"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug)   CONFIG="Debug"; shift ;;
        --release) CONFIG="Release"; shift ;;
        -h|--help) sed -n '2,8p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *)         echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

ARTEFACTS="$ROOT/build/Warehouse_artefacts/$CONFIG"

AU_SRC="$ARTEFACTS/AU/Warehouse.component"
VST3_SRC="$ARTEFACTS/VST3/Warehouse.vst3"

AU_DEST="$HOME/Library/Audio/Plug-Ins/Components"
VST3_DEST="$HOME/Library/Audio/Plug-Ins/VST3"

install_bundle() {
    local src="$1" dest_dir="$2" label="$3"

    if [[ ! -d "$src" ]]; then
        echo "skipping $label: not built at $src" >&2
        return 1
    fi

    mkdir -p "$dest_dir"

    local dest="$dest_dir/$(basename "$src")"

    rm -rf "$dest"
    cp -R "$src" "$dest"

    # Copying invalidates the signature, so re-sign at the destination.
    codesign --force --deep --sign - "$dest"

    echo "installed $label -> $dest"
}

status=0
install_bundle "$AU_SRC"   "$AU_DEST"   "AU"   || status=1
install_bundle "$VST3_SRC" "$VST3_DEST" "VST3" || status=1

if [[ $status -eq 0 ]]; then
    echo
    echo "Done. Restart your DAW to pick up the change."
    echo "If Logic does not see the AU, run: killall -9 AudioComponentRegistrar"
fi

exit $status
