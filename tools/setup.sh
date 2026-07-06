#!/usr/bin/env bash
# XOA checkout bootstrap — idempotent; safe to re-run any time.
#
#   ./tools/setup.sh
#
# Initialises the git submodules (JUCE, spatcore, hidapi — without them the
# build fails). Fixes a clone made without --recurse-submodules.

set -euo pipefail

cd "$(dirname "$0")/.."

echo "== XOA setup =="
echo "-- Initialising submodules (JUCE, spatcore, hidapi)..."
git submodule update --init --recursive

echo ""
echo "-- Done. Build with:"
echo "     cmake -S . -B build"
echo "     cmake --build build --config Release"
case "$(uname -s)" in
    Linux*)
        echo ""
        echo "   Linux build deps (Debian/Ubuntu):"
        echo "     sudo apt install build-essential cmake pkg-config libasound2-dev \\"
        echo "         libfreetype6-dev libfontconfig1-dev libgl1-mesa-dev \\"
        echo "         libcurl4-openssl-dev libgtk-3-dev libwebkit2gtk-4.1-dev libudev-dev"
        ;;
    Darwin*)
        echo "   (macOS: add -G Xcode for an Xcode project)"
        ;;
esac
