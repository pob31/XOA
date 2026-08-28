# Releasing XOA — signing, notarization and the CI pipeline

Covers WP14 (*Ship*) in [XOA-DEVPLAN.md](XOA-DEVPLAN.md): what a tagged release
produces, the one-time credential setup behind it, and the ritual to cut one.

## Workflows

| Workflow | Trigger | What it does |
|---|---|---|
| `.github/workflows/ci.yml` | push / PR to `main` | Debug build + ctest on the three OSes, plus the Linux-only OSC replay and GUI smoke gates. No secrets, no packaging. |
| `.github/workflows/headtrack-plugin.yml` | path-filtered push / PR | Compile-checks the out-of-tree webcam head-tracker plugin. |
| `.github/workflows/release.yml` | **Release published**, or manual dispatch against an existing tag | Release build on the three OSes, packages each, attaches every artifact to the GitHub Release. |

`release.yml` produces:

| OS | Asset | Signed? |
|---|---|---|
| macOS | `XOA-v<version>-macos-arm64.dmg` (+ `.sha256`) | **Yes** — Developer ID, hardened runtime, notarized and stapled |
| Windows | `XOA-<tag>-windows-x64-Setup.exe` (+ `.sha256`) | No — there is no Windows code-signing certificate; first run shows a SmartScreen prompt |
| Linux | `XOA-<tag>-linux-x86_64.tar.gz` (+ `.sha256`) | n/a |

(The DMG takes its version from `CMakeLists.txt`, the other two from the tag;
`verify-version` is what guarantees the two agree.)

The per-OS packaging logic lives in scripts, not in the YAML, so all three run
the same way locally:

- [`tools/ci/build-macos-release.sh`](../tools/ci/build-macos-release.sh) — build, sign, DMG, notarize, staple
- [`tools/linux/build-app-tarball.sh`](../tools/linux/build-app-tarball.sh) — tarball with `install.sh` / `uninstall.sh`
- [`Installer/XOA-Installer.iss`](../Installer/XOA-Installer.iss) — Inno Setup 6 script

---

## One-time setup: secrets

The five macOS secrets live in a **protected GitHub environment** named `xoa`
(repo → Settings → Environments), *not* at repository level. This repo is
public: the environment's required-reviewer rule is what stands between a
signing key and anything running off an untrusted ref. The `macos` job declares
`environment: xoa`, so it **pauses for an approval** on every release — that
pause in the Actions UI is expected behaviour, not a hung job.

| Secret | What it is |
|---|---|
| `MACOS_CERT_P12_BASE64` | "Developer ID Application" certificate **+ its private key**, exported as `.p12`, base64-encoded |
| `MACOS_CERT_PASSWORD` | the password that `.p12` was exported with |
| `NOTARY_API_KEY_BASE64` | App Store Connect API key (`AuthKey_XXXX.p8`), base64-encoded |
| `NOTARY_API_KEY_ID` | the key's **Key ID** (e.g. `ABCD1234EF`) |
| `NOTARY_API_ISSUER` | the **Issuer ID** (a UUID, on the same App Store Connect page) |

### The certificate `.p12`

Export it from Keychain Access — there is no CLI shortcut worth using, because
`security export` dumps **every** identity in the keychain, which would put your
unrelated "Apple Development" private key into the secret too.

1. Keychain Access → **login** keychain → **Certificates**.
2. Expand **"Developer ID Application: … (TEAMID)"** so its private key row
   appears. Select **both rows** → right-click → **Export 2 items…**
3. Save as `.p12` and set a password. **That password is `MACOS_CERT_PASSWORD`.**
4. Verify and upload both secrets in one step:

   ```bash
   tools/ci/set-macos-cert-secret.sh ~/path/to/DeveloperID.p12
   ```

   The script checks the bundle really contains a *Developer ID Application*
   certificate **and** a private key, prints its expiry, then sets
   `MACOS_CERT_P12_BASE64` and `MACOS_CERT_PASSWORD` together in the `xoa`
   environment. Setting them together is the point: a `.p12`/password mismatch
   is the most common way this pipeline fails, and it only fails inside the
   signing job, minutes into a release build. `--check` verifies without
   uploading; `--no-password` leaves `MACOS_CERT_PASSWORD` alone.

The certificate is **team-wide**: the same one used for WFS-DIY works here.

### The App Store Connect API key

The notary key is also team-wide — if one already exists for another project,
reuse those three values verbatim and skip this.

1. <https://appstoreconnect.apple.com> → **Users and Access → Integrations →
   App Store Connect API**.
2. Generate a key with **Developer** access, download `AuthKey_XXXX.p8`
   (downloadable **once**), and note the **Key ID** and **Issuer ID**.
3. ```bash
   base64 -i AuthKey_XXXX.p8 | pbcopy   # paste into NOTARY_API_KEY_BASE64
   ```

---

## Cutting a release

1. Bump the version in **`CMakeLists.txt`** — `project(XOA VERSION x.y.z …)` is
   the single source of truth. It feeds the bundle version, `XOA_VERSION_STRING`,
   the DMG/tarball names and the installer's `AppVersion`.
2. Commit and push to `main`.
3. Create a GitHub **Release** with tag **`v<version>`** (e.g. `v0.1.0`) and
   publish it. The `verify-version` job fails fast if the tag does not match the
   `project()` version, before any build minutes are spent.
4. **Approve the `xoa` environment deployment** when the macOS job requests it.
5. The three jobs build and attach the DMG, the installer and the tarball.

Re-running against an existing tag: Actions → **Release** → *Run workflow* →
enter the tag. Every job checks out that tag explicitly, so a dispatch from a
branch still builds what the tag says.

### Dry run — proving the pipeline before there is a v1 to cut

Actions → **Release** → *Run workflow*, leave **`tag` empty** and tick
**`dry_run`**. Every job builds, signs and packages exactly as it would for a
real release, but the assets land as **workflow artifacts** and no Release is
touched. Asset names fall back to `v<project version>` from `CMakeLists.txt`, and
`verify-version` reports the version instead of enforcing a tag match.

The macOS job still reads the signing secrets, so it still waits on the `xoa`
environment approval — which means a dry run exercises the credentials, the
notarization round-trip and the approval flow, not just the compile.

### Locally, without CI

```bash
tools/ci/build-macos-release.sh                  # needs the XOA-notary keychain profile
SKIP_NOTARIZE=1 tools/ci/build-macos-release.sh  # build + sign + DMG only
```

Set up the local notary profile once with
`xcrun notarytool store-credentials XOA-notary --key AuthKey_XXXX.p8 --key-id … --issuer …`.

---

## What ships, and what deliberately does not

- **macOS is arm64-only.** `ARCHS="arm64;x86_64" tools/ci/build-macos-release.sh`
  builds a universal binary and names the asset `…-macos-universal.dmg`; the
  workflow pins `arm64` because Intel Macs are not a target for this project and
  the second slice roughly doubles the macOS job. Changing it is a one-line edit
  to the `ARCHS` env in the `macos` job.
- **The webcam head-tracker plugin ships on Windows only.** There it is a
  self-contained `wfs_headtrack.dll` next to two prebuilt OpenCV DLLs. On
  macOS/Linux the plugin links the *system* OpenCV (brew / apt), whose dylib
  closure would have to be relocated, re-signed and redistributed — so the DMG
  and the tarball ship without it and head orientation stays on manual/OSC
  control. Closing that gap means porting WFS-DIY's
  `WFS_HEADTRACK_BUNDLED_OPENCV` option (a FetchContent'd minimal **static**
  OpenCV built into the plugin) into `tools/headtrack/CMakeLists.txt`. The
  Windows plugin step is `continue-on-error` and the `.iss` marks every plugin
  file `skipifsourcedoesntexist`, so a plugin break degrades the installer
  rather than blocking the release; the job summary states which way it went.
- **No GPU plugin packaging.** Unlike WFS-DIY, XOA does not build the per-vendor
  `libwfs_<vendor>` plugins yet (spatcore GPU is Track D / WP11). When it does,
  the packaging hooks go in the same three scripts.
- **Windows is unsigned** by design.
- **The Linux udev rules ship ahead of the feature.**
  `assets/linux/70-xoa.rules` grants non-root hidraw access to the Elgato
  Stream Deck+ and the 3Dconnexion SpaceMouse; `install.sh` offers to drop them
  in `/etc/udev/rules.d` and the uninstaller removes them. XOA links
  `spatcore-controllers` but no source drives either device yet — the rules only
  relax permissions, so they are inert until that support lands and the packaging
  does not need revisiting when it does.
- **The `.xoa` file association** (Windows) opens the project folder the manifest
  belongs to — `AppShell::applyStartupCommandLine` takes a project path as its
  first command-line token.

## Gotchas worth not rediscovering

- **Data files may not live in `Contents/MacOS`.** codesign treats that
  directory as code, so `codesign --verify --strict` fails with *"code object is
  not signed at all"* on each staged data file and the notary rejects the build.
  The CMake POST_BUILD steps stage into `Contents/Resources` on macOS and beside
  the binary elsewhere; `LocalizationManager::getResourceDirectory()` and
  `XoaMonitoringEngine::builtInSofaFile()` mirror that split, and
  `build-macos-release.sh` fails fast if a `Contents/MacOS/Resources` tree
  reappears.
- **A stale build tree hides that.** The staging is a copy, not a sync, so an
  old tree keeps files at the previous location. `rm -rf build-release` when a
  staging path changes.

## Known iteration points

- The `headtrack-plugin.yml` OpenCV cache uses `actions/cache@v4`; the release
  workflow matches it so both share one cache entry. If that action major stops
  resolving, bump **both** together or the release job silently re-downloads the
  180 MB prebuilt.
- The per-OS artefact paths (`build-release/XOA_artefacts/Release/…`) are a first
  cut — they follow JUCE's CMake layout and the scripts locate the binary with a
  `find` rather than a hard-coded path, but check the Actions log after the first
  real run.
- `spatcore` and `JUCE` are submodules; every job checks out with
  `submodules: recursive`. Both are public, so the default `GITHUB_TOKEN`
  fetches them.
