#!/usr/bin/env bash
#
# Fill the macOS code-signing secrets the Release workflow consumes:
#
#   MACOS_CERT_P12_BASE64   Developer ID Application cert + private key, .p12, base64
#   MACOS_CERT_PASSWORD     the password that .p12 was exported with
#
# Both live in the PROTECTED GitHub environment (default: `xoa`), NOT at repo
# level — the repository is public and the environment's reviewer rule is what
# stops a fork PR from ever reaching the signing job.
#
# Usage:
#   tools/ci/set-macos-cert-secret.sh <path-to.p12>          # verify + upload both
#   tools/ci/set-macos-cert-secret.sh <path-to.p12> --check  # verify only, no upload
#   tools/ci/set-macos-cert-secret.sh <path-to.p12> --no-password
#                                                           # leave MACOS_CERT_PASSWORD alone
#
# Env overrides: REPO (default pob31/XOA), GH_ENV (default xoa).
#
# The password is read from the tty with echo off and never appears in argv,
# the environment, or the shell history. The base64 payload is piped to `gh`
# on stdin for the same reason.
#
# Producing the .p12 (one time, Keychain Access — there is no CLI path that
# exports a SINGLE identity; `security export` dumps every identity in the
# keychain, which would put your Apple Development key in the secret too):
#   1. Keychain Access -> login -> Certificates.
#   2. Expand "Developer ID Application: <you> (TEAMID)" so its private key
#      shows, select BOTH rows, right-click -> "Export 2 items…".
#   3. Save as .p12 and set a password — that password is MACOS_CERT_PASSWORD.
#
# Notarization needs three more secrets (App Store Connect API key); those are
# a separate one-time step, see Documentation/XOA-RELEASE.md.

set -euo pipefail

REPO="${REPO:-pob31/XOA}"
GH_ENV="${GH_ENV:-xoa}"

P12=""
SET_PASSWORD=1
CHECK_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --no-password) SET_PASSWORD=0 ;;
        --check)       CHECK_ONLY=1 ;;
        -h|--help)     sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*)            echo "error: unknown option $arg" >&2; exit 2 ;;
        *)             P12="$arg" ;;
    esac
done

if [ -z "$P12" ]; then
    echo "usage: $0 <path-to.p12> [--check] [--no-password]" >&2
    exit 2
fi
[ -f "$P12" ] || { echo "error: no such file: $P12" >&2; exit 1; }

# ── Password (tty only) ───────────────────────────────────────────────────
printf 'Password for %s: ' "$(basename "$P12")" >&2
IFS= read -r -s PW
printf '\n' >&2
[ -n "$PW" ] || { echo "error: empty password" >&2; exit 1; }

# ── Verify the bundle before it becomes a secret ──────────────────────────
# openssl 3 (brew) refuses the legacy RC2 encryption Keychain Access still
# writes unless -legacy is passed; LibreSSL (/usr/bin/openssl) rejects the
# flag instead. Try plain first, then legacy.
p12_dump() {
    openssl pkcs12 -in "$P12" -passin env:P12_PW "$@" 2>/dev/null \
        || openssl pkcs12 -in "$P12" -passin env:P12_PW -legacy "$@" 2>/dev/null
}
export P12_PW="$PW"

CERTS="$(p12_dump -nokeys || true)"
if [ -z "$CERTS" ]; then
    echo "error: could not open $P12 - wrong password, or an unsupported PKCS#12 encoding." >&2
    exit 1
fi

# Enumerate every certificate in the bundle (a "2 items" export holds one, but
# an export of several identities holds more, and only one of them is the
# Developer ID Application leaf we sign with).
SUBJECTS="$(printf '%s\n' "$CERTS" \
    | openssl crl2pkcs7 -nocrl -certfile /dev/stdin 2>/dev/null \
    | openssl pkcs7 -print_certs -noout 2>/dev/null \
    | sed -n 's/^subject=//p')"
echo "== certificates in the bundle"
printf '%s\n' "$SUBJECTS" | sed 's/^/   /'

if ! printf '%s\n' "$SUBJECTS" | grep -q "Developer ID Application"; then
    echo "error: no 'Developer ID Application' certificate in $P12." >&2
    echo "       The app and DMG are signed with that identity; re-export it" >&2
    echo "       from Keychain Access (see the header of this script)." >&2
    exit 1
fi

if ! p12_dump -nocerts -nodes | grep -q "PRIVATE KEY"; then
    echo "error: $P12 has no private key - you exported the certificate alone." >&2
    echo "       Expand the certificate row in Keychain Access and export BOTH items." >&2
    exit 1
fi
echo "== ok: Developer ID Application certificate + private key present"

# Expiry: a cert that dies mid-release-cycle is a silent time bomb. Report the
# leaf's date (the -clcerts view is the cert that owns the private key).
NOTAFTER="$(p12_dump -nokeys -clcerts | openssl x509 -noout -enddate 2>/dev/null | cut -d= -f2- || true)"
[ -n "$NOTAFTER" ] && echo "   expires: $NOTAFTER"

if [ "$CHECK_ONLY" = 1 ]; then
    echo "== --check given: nothing uploaded"
    exit 0
fi

# ── Upload ────────────────────────────────────────────────────────────────
command -v gh >/dev/null || { echo "error: gh CLI not found" >&2; exit 1; }

echo "== setting MACOS_CERT_P12_BASE64 on $REPO (environment: $GH_ENV)"
base64 < "$P12" | tr -d '\n' | gh secret set MACOS_CERT_P12_BASE64 -R "$REPO" --env "$GH_ENV"

if [ "$SET_PASSWORD" = 1 ]; then
    # Set it from the SAME prompt that just verified the file: a p12/password
    # mismatch is the most common way this pipeline fails, and it only fails
    # inside the signing job, ten minutes into a release build.
    echo "== setting MACOS_CERT_PASSWORD (same password, kept in sync)"
    printf '%s' "$PW" | gh secret set MACOS_CERT_PASSWORD -R "$REPO" --env "$GH_ENV"
fi

unset P12_PW PW
echo
gh secret list -R "$REPO" --env "$GH_ENV"
