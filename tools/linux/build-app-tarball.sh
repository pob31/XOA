#!/usr/bin/env bash
# Build the Linux release tarball for the XOA app.
#
# Usage:
#   tools/linux/build-app-tarball.sh
#
# Produces:
#   <OUTPUT_DIR>/<TARBALL_NAME>.tar.gz
#   default: build-release/package/XOA-v<version>-linux-<arch>.tar.gz
#
# Env overrides (the release workflow uses them to match the asset naming
# convention shared with the macOS DMG and the Windows installer):
#   BUILD_DIR     CMake binary dir to package from   (default: build-release)
#   OUTPUT_DIR    where to write the tarball         (default: $BUILD_DIR/package)
#   TARBALL_NAME  archive basename + top-level dir   (default: XOA-v<version>-linux-<arch>)
#
# The tarball carries the binary, the runtime data it resolves beside itself
# (Resources/lang, Resources/SOFA), the HID controller udev rules, a .desktop
# entry, install.sh / uninstall.sh and the legal texts. It is the Linux
# counterpart of the macOS .dmg and the Windows Inno Setup .exe.
#
# Pre-requisite: a Release build must already exist. This script does not run
# CMake itself - keeping build and package split lets CI parallelise them, and
# lets you re-package without a rebuild.
#
# NOT bundled: the webcam head-tracker plugin (libwfs_headtrack.so). It needs a
# statically linked OpenCV to be redistributable, which tools/headtrack does not
# build yet - see Documentation/XOA-RELEASE.md. Head tracking stays on manual
# orientation unless the user builds the plugin themselves.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build-release}"
[[ "$BUILD_DIR" = /* ]] || BUILD_DIR="$REPO_ROOT/$BUILD_DIR"
RELEASE_DIR="${OUTPUT_DIR:-$BUILD_DIR/package}"

# Version: single source of truth is the CMake project() call.
VERSION="$(sed -nE 's/^project\(XOA VERSION ([0-9][^ )]*).*/\1/p' "$REPO_ROOT/CMakeLists.txt" | head -n1)"
if [[ -z "$VERSION" ]]; then
    echo "ERROR: could not read the version from CMakeLists.txt" >&2
    exit 1
fi

# JUCE puts the artefact in <build>/XOA_artefacts/<Config>/ - find it rather
# than hard-coding the config, so a locally configured RelWithDebInfo tree
# packages too. -perm -u+x excludes the same-named artefacts directory.
BIN="$(find "$BUILD_DIR/XOA_artefacts" -maxdepth 2 -type f -name XOA -perm -u+x 2>/dev/null | head -n1)"
if [[ -z "$BIN" ]]; then
    echo "ERROR: no XOA binary under $BUILD_DIR/XOA_artefacts." >&2
    echo "       Build first: cmake -S . -B $(basename "$BUILD_DIR") -DCMAKE_BUILD_TYPE=Release" >&2
    echo "                    cmake --build $(basename "$BUILD_DIR") --parallel \$(nproc)" >&2
    exit 1
fi

ARCH="$(uname -m)"
STAGE_NAME="${TARBALL_NAME:-XOA-v${VERSION}-linux-${ARCH}}"
STAGE_DIR="$RELEASE_DIR/$STAGE_NAME"
TARBALL="$RELEASE_DIR/$STAGE_NAME.tar.gz"

echo "==> Staging $STAGE_NAME (from $BIN)"
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR/Resources"

cp "$BIN" "$STAGE_DIR/XOA"
chmod +x "$STAGE_DIR/XOA"

# Runtime data, read from the source-of-truth directories rather than from the
# build output: the CMake POST_BUILD staging can hold stale copies from an
# earlier configure, and the tarball must not inherit those.
cp -R "$REPO_ROOT/Resources/lang" "$STAGE_DIR/Resources/lang"
cp -R "$REPO_ROOT/assets/SOFA"    "$STAGE_DIR/Resources/SOFA"
[[ -f "$STAGE_DIR/Resources/lang/en.json" ]] || { echo "ERROR: lang files missing" >&2; exit 1; }
compgen -G "$STAGE_DIR/Resources/SOFA/*.sofa" >/dev/null \
    || { echo "ERROR: the bundled HRTF set is missing" >&2; exit 1; }

cp "$REPO_ROOT/LICENSE"                "$STAGE_DIR/LICENSE"
cp "$REPO_ROOT/THIRD_PARTY_NOTICES.md" "$STAGE_DIR/THIRD_PARTY_NOTICES.txt"

# .desktop entry - install.sh rewrites Exec= to the chosen prefix. No Icon= key:
# the app ships no icon asset yet, and pointing at a missing file gives a broken
# launcher entry rather than the desktop's generic fallback.
mkdir -p "$STAGE_DIR/share"

# udev rules for the HID controllers (Stream Deck+, SpaceMouse). They ship
# ahead of the app's controller support and are inert until it lands - see the
# header of the rules file. install.sh offers to drop them in /etc/udev/rules.d.
cp "$REPO_ROOT/assets/linux/70-xoa.rules" "$STAGE_DIR/share/"

cat > "$STAGE_DIR/share/xoa.desktop" <<'DESKTOP_EOF'
[Desktop Entry]
Type=Application
Name=XOA
GenericName=Ambisonics Spatial Audio Processor
Comment=Tenth-order Ambisonics encoding, transformation and decoding
Exec=__PREFIX__/bin/XOA
Terminal=false
Categories=AudioVideo;Audio;
StartupNotify=true
DESKTOP_EOF

# ── install.sh ────────────────────────────────────────────────────────────
cat > "$STAGE_DIR/install.sh" <<'INSTALL_EOF'
#!/usr/bin/env bash
# XOA installer.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"

MODE="${1:-}"
case "$MODE" in
    --user)   PREFIX="$HOME/.local"; SUDO="" ;;
    --system) PREFIX="/opt/xoa";     SUDO="sudo" ;;
    *)
        echo "Usage: $0 [--user | --system]"
        echo
        echo "  --user    Install for the current user only (~/.local), no sudo."
        echo "  --system  Install system-wide to /opt/xoa (requires sudo)."
        if [[ -t 0 ]]; then
            read -r -p "Install for the current user only? [Y/n] " ans
            case "${ans,,}" in
                n|no) MODE="--system"; PREFIX="/opt/xoa"; SUDO="sudo" ;;
                *)    MODE="--user";   PREFIX="$HOME/.local"; SUDO="" ;;
            esac
        else
            exit 1
        fi
        ;;
esac

BIN_DIR="$PREFIX/bin"
APP_DIR="$PREFIX/share/xoa"
DESKTOP_DIR="$PREFIX/share/applications"

echo "==> Installing to $PREFIX (mode: $MODE)"
$SUDO mkdir -p "$BIN_DIR" "$APP_DIR" "$DESKTOP_DIR"

# The binary lives in APP_DIR with its Resources/ tree beside it: XOA resolves
# Resources/lang and Resources/SOFA relative to its own executable. bin/XOA is a
# symlink - the app reads /proc/self/exe, which resolves it, so the lookup still
# lands in APP_DIR.
$SUDO install -m 0755 "$HERE/XOA" "$APP_DIR/XOA"
$SUDO rm -rf "$APP_DIR/Resources"
$SUDO cp -R  "$HERE/Resources" "$APP_DIR/Resources"
$SUDO install -m 0644 "$HERE/LICENSE"                 "$APP_DIR/LICENSE"
$SUDO install -m 0644 "$HERE/THIRD_PARTY_NOTICES.txt" "$APP_DIR/THIRD_PARTY_NOTICES.txt"
$SUDO ln -sf "$APP_DIR/XOA" "$BIN_DIR/XOA"

# Uninstaller with the prefix baked in, so nobody has to remember which prefix
# they installed against. The inner heredoc is UNQUOTED: $PREFIX expands now and
# is baked in, while \$PREFIX / \$SUDO / \$HOME stay literal for uninstall time.
$SUDO tee "$APP_DIR/uninstall.sh" >/dev/null <<UNINSTALL_INNER
#!/usr/bin/env bash
set -euo pipefail
PREFIX="$PREFIX"
SUDO=""; [[ "\$PREFIX" != "\$HOME/"* ]] && SUDO="sudo"

\$SUDO rm -f "\$PREFIX/bin/XOA"
\$SUDO rm -f "\$PREFIX/share/applications/xoa.desktop"
if command -v update-desktop-database >/dev/null 2>&1; then
    \$SUDO update-desktop-database "\$PREFIX/share/applications" || true
fi
if [[ -f /etc/udev/rules.d/70-xoa.rules ]]; then
    sudo rm -f /etc/udev/rules.d/70-xoa.rules
    sudo udevadm control --reload-rules || true
fi
# The share dir goes last: this deletes the running script, but bash has already
# read it, so the remaining lines still execute.
\$SUDO rm -rf "\$PREFIX/share/xoa"
echo "Uninstalled XOA from \$PREFIX."
echo "Settings and projects under ~/.config and your project folders are left untouched."
UNINSTALL_INNER
$SUDO chmod +x "$APP_DIR/uninstall.sh"

sed "s|__PREFIX__|$PREFIX|g" "$HERE/share/xoa.desktop" \
    | $SUDO tee "$DESKTOP_DIR/xoa.desktop" >/dev/null
if command -v update-desktop-database >/dev/null 2>&1; then
    $SUDO update-desktop-database "$DESKTOP_DIR" || true
fi

# udev rules for the HID controllers. Always system-wide (/etc/udev/rules.d) no
# matter which prefix was chosen, so they need sudo even for a --user install -
# hence the separate prompt rather than doing it unasked.
echo
if [[ -t 0 ]]; then
    read -r -p "Install udev rules for HID controllers (Stream Deck+, SpaceMouse)? Needs sudo. [Y/n] " ans
    case "${ans,,}" in
        n|no) ;;
        *)
            sudo install -m 0644 "$HERE/share/70-xoa.rules" /etc/udev/rules.d/70-xoa.rules
            sudo udevadm control --reload-rules
            sudo udevadm trigger
            echo "    udev rules installed. Replug the device to apply."
            ;;
    esac
else
    echo "Stdin is not a tty - skipping the udev rules. Install them by hand with:"
    echo "    sudo install -m 0644 $HERE/share/70-xoa.rules /etc/udev/rules.d/"
fi

echo
echo "Done. Launch from your application menu, or run: $BIN_DIR/XOA"
if [[ "$MODE" == "--user" ]] && [[ ":$PATH:" != *":$BIN_DIR:"* ]]; then
    echo "Note: $BIN_DIR is not on your PATH."
fi
echo "To uninstall: ${SUDO:+$SUDO }$APP_DIR/uninstall.sh"
INSTALL_EOF
chmod +x "$STAGE_DIR/install.sh"

# ── uninstall.sh (the in-tarball copy, for a prefix given by hand) ────────
cat > "$STAGE_DIR/uninstall.sh" <<'UNINSTALL_EOF'
#!/usr/bin/env bash
# Remove an XOA install. Pass the prefix you installed to; defaults to ~/.local.
# (install.sh also writes a prefix-baked uninstall.sh into the install dir -
# prefer that one, it needs no argument.)
set -euo pipefail
PREFIX="${1:-$HOME/.local}"
SUDO=""; [[ "$PREFIX" != "$HOME/"* ]] && SUDO="sudo"

$SUDO rm -f  "$PREFIX/bin/XOA"
$SUDO rm -f  "$PREFIX/share/applications/xoa.desktop"
$SUDO rm -rf "$PREFIX/share/xoa"
if command -v update-desktop-database >/dev/null 2>&1; then
    $SUDO update-desktop-database "$PREFIX/share/applications" || true
fi
if [[ -f /etc/udev/rules.d/70-xoa.rules ]]; then
    sudo rm -f /etc/udev/rules.d/70-xoa.rules
    sudo udevadm control --reload-rules || true
fi
echo "Uninstalled XOA from $PREFIX."
UNINSTALL_EOF
chmod +x "$STAGE_DIR/uninstall.sh"

# ── README ────────────────────────────────────────────────────────────────
cat > "$STAGE_DIR/README.txt" <<README_EOF
XOA ${VERSION} — Linux ${ARCH}

Tenth-order Ambisonics spatial audio processor.

Quick install
    ./install.sh --user      # ~/.local, no sudo
    ./install.sh --system    # /opt/xoa, sudo

Or run in place
    ./XOA

The app reads Resources/lang (translations) and Resources/SOFA (the bundled
SADIE II KU100 HRTF set used by the binaural monitor) from beside the binary,
which is how they are laid out here. install.sh keeps that layout.

Runtime requirements
    ALSA, and JACK if you use the JACK device type (libjack). PipeWire's JACK
    shim works. OpenGL for the 3D map view.

HID controllers
    share/70-xoa.rules grants non-root access to the Elgato Stream Deck+ and
    3Dconnexion SpaceMouse. install.sh offers to install them. They ship ahead
    of the app's controller support and do nothing until it lands.

Uninstall
    ./uninstall.sh [prefix]     # or the uninstall.sh install.sh drops in place

Webcam head tracking is not included in this archive: the plugin needs a
statically linked OpenCV to be redistributable. Build it yourself with
tools/headtrack/build-headtrack-plugin.sh and drop libwfs_headtrack.so next to
the binary. Without it, head orientation stays manual/OSC-driven.

Source and issues: https://github.com/pob31/XOA
Licensed under the GNU GPL v3 — see LICENSE and THIRD_PARTY_NOTICES.txt.
README_EOF

# ── Archive ───────────────────────────────────────────────────────────────
mkdir -p "$RELEASE_DIR"
cd "$RELEASE_DIR"
echo "==> Creating $TARBALL"
tar --owner=0 --group=0 -czf "$TARBALL" "$STAGE_NAME"
rm -rf "$STAGE_DIR"

echo
ls -lh "$TARBALL"
echo "Done."
