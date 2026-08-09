#!/usr/bin/env bash
# Build the wfs_headtrack webcam head-tracking plugin (Linux/macOS) and stage
# it next to the app. OpenCV comes from the system package manager:
#   Linux:  sudo apt install libopencv-dev
#   macOS:  brew install opencv
#
# Usage: tools/headtrack/build-headtrack-plugin.sh [Release|Debug] [stageDir]

set -euo pipefail

CONFIG="${1:-Release}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="$SCRIPT_DIR/build"

cmake -S "$SCRIPT_DIR" -B "$BUILD" -DCMAKE_BUILD_TYPE="$CONFIG"
cmake --build "$BUILD" --parallel

if [[ "$(uname)" == "Darwin" ]]; then
    LIB="libwfs_headtrack.dylib"
else
    LIB="libwfs_headtrack.so"
fi

OUT="$BUILD"
[[ -f "$BUILD/$LIB" ]] || OUT="$BUILD/$CONFIG"

# Export self-check: a plugin without its ABI symbol would be silently useless.
if command -v nm >/dev/null 2>&1; then
    if ! nm -D --defined-only "$OUT/$LIB" 2>/dev/null | grep -q wfs_headtrack_abi_version \
       && ! nm -gU "$OUT/$LIB" 2>/dev/null | grep -q wfs_headtrack_abi_version; then
        echo "WARNING: $LIB is missing wfs_headtrack_abi_version - the app would not load it" >&2
    fi
fi

STAGE="${2:-}"
if [[ -z "$STAGE" ]]; then
    # XOA is CMake-native: the app lands in build/XOA_artefacts/<Config>.
    if [[ "$(uname)" == "Darwin" ]]; then
        STAGE="$ROOT/build/XOA_artefacts/$CONFIG/XOA.app/Contents/MacOS"
    else
        STAGE="$ROOT/build/XOA_artefacts/$CONFIG"
    fi
fi

if [[ -d "$STAGE" ]]; then
    cp "$OUT/$LIB" "$STAGE/"
    cp "$OUT/face_detection_yunet_2023mar.onnx" "$STAGE/"
    echo "Staged $LIB + model into $STAGE"
else
    echo "App output dir not found ($STAGE) - plugin left in $OUT"
fi

echo "OK: $OUT/$LIB"
