#!/usr/bin/env bash
#
# Assert that every architecture slice of every named Mach-O declares the intended
# LC_BUILD_VERSION `minos`, and that a universal build really produced two slices.
#
# THE DEPLOYMENT TARGET HAS TO BE ASSERTED, NOT TRUSTED.
#
# CMakeLists.txt sets CMAKE_OSX_DEPLOYMENT_TARGET to 11.0, and for the whole of v1.0.0 that line
# did nothing: project() has already created the cache entry, so a set(... CACHE ...) without FORCE
# is a silent no-op. Every bundle was stamped with the BUILD MACHINE'S SDK version instead --
# measured at `minos 26.0` on a macOS 26.5 SDK, i.e. a plugin that refuses to load on every macOS
# version the README claims to support, including both slices of the universal build whose entire
# purpose is older Intel Macs.
#
# Nothing in the source, the build log, the test suite or the packaging step could see it. The only
# place the real value appears is the Mach-O load command, so that is what this reads. The fix and
# this assertion are one change: without the assertion the same defect returns the next time
# someone touches that block, and it returns invisibly.
#
# ONE SCRIPT, TWO CALLERS (.github/workflows/ci.yml and release.yml). This check was copy-pasted
# between those two workflows and the copies had already drifted apart -- only one of them counted
# the slices, though both configure -DUNIVERSAL_BINARY=ON. A check whose entire job is to catch an
# invisible defect is the last thing that should exist in two versions.
#
# Usage: scripts/assert-min-macos.sh <expected-minos> <mach-o> [mach-o...]

set -uo pipefail

if [ "$#" -lt 2 ]; then
    echo "usage: $0 <expected-minos> <mach-o> [mach-o...]" >&2
    exit 2
fi

EXPECTED="$1"
shift

fail=0

for bundle in "$@"; do
    if [ ! -f "$bundle" ]; then
        echo "::error::$bundle not found"
        fail=1
        continue
    fi

    # One `minos` line per architecture slice, so a universal binary prints two and BOTH must
    # match -- an x86_64 slice stamped with a modern minos is exactly as unloadable as an arm64
    # one, and is the slice most likely to be running an older macOS.
    slices="$(otool -l "$bundle" | awk '/minos/ { print $2 }')"

    if [ -z "$slices" ]; then
        echo "::error::$bundle declares no LC_BUILD_VERSION minos at all"
        fail=1
        continue
    fi

    count=0

    for minos in $slices; do
        count=$((count + 1))

        if [ "$minos" = "$EXPECTED" ]; then
            echo "ok   $bundle slice $count minos=$minos"
        else
            echo "::error::$bundle slice $count has minos=$minos, expected $EXPECTED."
            echo "         CMAKE_OSX_DEPLOYMENT_TARGET did not apply -- check that the"
            echo "         set() in CMakeLists.txt still carries FORCE."
            fail=1
        fi
    done

    # Both callers configure UNIVERSAL_BINARY=ON, so anything other than two slices means the fat
    # build silently degraded and the per-slice assertion above only covered half of what ships.
    if [ "$count" -ne 2 ]; then
        echo "::error::$bundle has $count architecture slice(s), expected 2 (arm64 + x86_64)"
        fail=1
    fi
done

exit $fail
