#!/usr/bin/env bash
#
# Build, sign, notarize, staple and package a distributable macOS release of XOA.
# Runs identically locally and in CI (GitHub Actions, macos-latest).
#
# Produces, under dist/:
#   XOA-v<version>-macos-arm64.dmg          signed + notarized + stapled
#   XOA-v<version>-macos-arm64.dmg.sha256   checksum
#   XOA.app                                 the signed + stapled bundle
#
# Usage:
#   tools/ci/build-macos-release.sh                 # full build + sign + notarize
#   SKIP_BUILD=1    tools/ci/build-macos-release.sh # reuse the existing Release build
#   SKIP_NOTARIZE=1 tools/ci/build-macos-release.sh # build + sign + dmg, no notarize
#
# Env:
#   ARCHS           CMAKE_OSX_ARCHITECTURES value (default arm64; "arm64;x86_64"
#                   builds a universal binary - see Documentation/XOA-RELEASE.md).
#   DEVELOPER_ID    signing identity; default = the sole "Developer ID Application"
#                   in the keychain (CI imports it via apple-actions/import-codesign-certs).
#   BUILD_DIR       CMake binary dir (default: build-release).
#
# Notarization credentials, resolved in this order:
#   1. App Store Connect API key (what CI uses):
#        NOTARY_API_KEY (path to the .p8) + NOTARY_API_KEY_ID + NOTARY_API_ISSUER
#   2. Apple-ID app-specific password:
#        NOTARY_APPLE_ID + NOTARY_PASSWORD + NOTARY_TEAM_ID
#   3. Local fallback: the notarytool keychain profile $NOTARY_PROFILE (XOA-notary).

set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_DIR"

APP_NAME="XOA"
BUILD_DIR="${BUILD_DIR:-build-release}"
ARCHS="${ARCHS:-arm64}"
ENTITLEMENTS="tools/ci/release-entitlements.plist"
NOTARY_PROFILE="${NOTARY_PROFILE:-XOA-notary}"

# ── Version — single source of truth: the CMake project() call ────────────
VERSION="$(sed -nE 's/^project\(XOA VERSION ([0-9][^ )]*).*/\1/p' CMakeLists.txt | head -n1)"
[ -n "$VERSION" ] || { echo "error: could not read the version from CMakeLists.txt" >&2; exit 1; }
# The arch tag is part of the asset name: an arm64-only DMG must not look like
# something an Intel Mac can run.
case "$ARCHS" in
    arm64)  ARCH_TAG="arm64" ;;
    x86_64) ARCH_TAG="x86_64" ;;
    *)      ARCH_TAG="universal" ;;
esac
PKG="${APP_NAME}-v${VERSION}-macos-${ARCH_TAG}"
echo "==> $APP_NAME v${VERSION} ($ARCHS)"

# ── Signing identity ──────────────────────────────────────────────────────
if [ -z "${DEVELOPER_ID:-}" ]; then
    DEVELOPER_ID="$(security find-identity -v -p codesigning \
        | sed -nE 's/.*"(Developer ID Application: .*)"/\1/p' | head -n1)"
fi
[ -n "$DEVELOPER_ID" ] || { echo "error: no 'Developer ID Application' identity found" >&2; exit 1; }
echo "    signing identity: $DEVELOPER_ID"

# ── Build ─────────────────────────────────────────────────────────────────
# A dedicated binary dir, not the day-to-day build/: release builds are Release
# + arch-pinned, and reusing a Debug tree here has produced signed Debug DMGs
# elsewhere. spatcore_apply_compile_flags() supplies the optimization/FP flags.
if [ "${SKIP_BUILD:-0}" != "1" ]; then
    echo "==> Configuring + building Release"
    cmake -S . -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="$ARCHS"
    cmake --build "$BUILD_DIR" --config Release --parallel "$(sysctl -n hw.ncpu)"
fi

APP="$(find "$BUILD_DIR" -maxdepth 4 -name "${APP_NAME}.app" -type d | head -n1)"
[ -n "$APP" ] && [ -d "$APP" ] || {
    echo "error: ${APP_NAME}.app not found under $BUILD_DIR" >&2; exit 1; }
echo "    app bundle: $APP"

# On macOS the app resolves lang/ and SOFA/ under Contents/Resources, and CMake's
# POST_BUILD steps stage them there. Missing means the app ships with no
# translations and no fallback HRTF set - a shipping bug, not a warning - so
# fail here rather than notarize a broken bundle.
for r in Resources/lang/en.json Resources/SOFA; do
    [ -e "$APP/Contents/$r" ] || {
        echo "error: $r missing from the bundle - the CMake staging step did not run" >&2
        exit 1; }
done

# Contents/MacOS is the directory codesign treats as CODE. A data tree staged
# there makes `codesign --verify --strict` fail with "code object is not signed
# at all" on every file in it, and the notary rejects the result. That is
# exactly how the resources used to be staged, so guard the regression here
# rather than discover it again ten minutes into a release.
if [ -d "$APP/Contents/MacOS/Resources" ]; then
    echo "error: data staged in Contents/MacOS/Resources - it belongs in Contents/Resources." >&2
    echo "       (Stale output from an older build? Remove $BUILD_DIR and rebuild.)" >&2
    exit 1
fi

# ── Re-sign: hardened runtime + our entitlements ──────────────────────────
echo "==> Signing with $ENTITLEMENTS"
codesign --force --options runtime --timestamp \
    --entitlements "$ENTITLEMENTS" \
    --sign "$DEVELOPER_ID" "$APP"
codesign --verify --strict --verbose=2 "$APP"
# Guard the entitlement that breaks notarization (notary statusCode 4000).
if codesign -d --entitlements - "$APP" 2>/dev/null | grep -A1 get-task-allow | grep -qi true; then
    echo "error: get-task-allow is still true after signing - notarization would fail" >&2
    exit 1
fi

# ── Stage + build the DMG ─────────────────────────────────────────────────
mkdir -p dist
DMG="dist/${PKG}.dmg"
echo "==> Creating $DMG"
rm -f "$DMG"
STAGE_ROOT="$(mktemp -d)"
STAGE="$STAGE_ROOT/dmg"
mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/"
cp LICENSE "$STAGE/LICENSE"
cp README.md "$STAGE/README.txt"
cp THIRD_PARTY_NOTICES.md "$STAGE/THIRD_PARTY_NOTICES.txt"
ln -s /Applications "$STAGE/Applications"

# hdiutil intermittently fails with "Resource busy" on CI - a stale same-name
# volume still attached, or Spotlight walking the fresh tree. Detach and retry.
make_dmg() { hdiutil create -volname "$APP_NAME" -srcfolder "$STAGE" -ov -format UDZO "$DMG"; }
attempt=0
until make_dmg >/dev/null 2>&1; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 5 ]; then make_dmg >&2; exit 1; fi
    echo "    hdiutil busy - detach + retry $attempt/5"
    hdiutil detach "/Volumes/$APP_NAME" >/dev/null 2>&1 || true
    sleep 5
done
rm -rf "$STAGE_ROOT"

echo "==> Signing DMG"
codesign --force --timestamp --sign "$DEVELOPER_ID" "$DMG"

# ── Notarize + staple ─────────────────────────────────────────────────────
if [ -n "${NOTARY_API_KEY:-}" ] && [ -n "${NOTARY_API_KEY_ID:-}" ] && [ -n "${NOTARY_API_ISSUER:-}" ]; then
    NOTARY_AUTH=(--key "$NOTARY_API_KEY" --key-id "$NOTARY_API_KEY_ID" --issuer "$NOTARY_API_ISSUER")
elif [ -n "${NOTARY_APPLE_ID:-}" ] && [ -n "${NOTARY_PASSWORD:-}" ] && [ -n "${NOTARY_TEAM_ID:-}" ]; then
    NOTARY_AUTH=(--apple-id "$NOTARY_APPLE_ID" --password "$NOTARY_PASSWORD" --team-id "$NOTARY_TEAM_ID")
else
    NOTARY_AUTH=(--keychain-profile "$NOTARY_PROFILE")
fi

if [ "${SKIP_NOTARIZE:-0}" != "1" ]; then
    echo "==> Submitting to the Apple notary service"
    xcrun notarytool submit "$DMG" "${NOTARY_AUTH[@]}" --wait
    echo "==> Stapling"
    xcrun stapler staple "$DMG"
    # Staple the .app too, so a copy dragged out of the DMG validates offline.
    xcrun stapler staple "$APP" || true
    spctl -a -t open --context context:primary-signature -vvv "$DMG" || true
    xcrun stapler validate "$DMG"
fi

# ── Checksum + final copy ─────────────────────────────────────────────────
rm -rf "dist/${APP_NAME}.app"
cp -R "$APP" "dist/${APP_NAME}.app"
shasum -a 256 "$DMG" | awk '{print $1}' > "${DMG}.sha256"
echo "==> Done: $DMG"
ls -lh "$DMG" "${DMG}.sha256"
