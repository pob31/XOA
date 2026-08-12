# XOA — Claude Code notes

Tenth-order Ambisonics (10OA) spatial audio processor. The Ambisonics sibling
of WFS-DIY (`d:/dev/WFS_DIY_v1`), consuming the same shared engine —
**spatcore** — as a git submodule, but **CMake-native** (no Projucer; spatcore's
CMakeLists names XOA as exactly this kind of consumer).

## Build

```
cmake -S . -B build
cmake --build build --config Release     # or Debug
```

Binary: `build/XOA_artefacts/<config>/XOA.exe`. After a fresh clone run
`./tools/setup.sh` (initialises submodules).

## Repo shape

- `Source/` — the XOA application (JUCE gui app). App layer only.
- `spatcore/` — **submodule**, pinned to **47b1ae5** (tag `v0.2.0`: GPU
  node-parallel SDN, Max-port FR diffusion, MCP protocol negotiation, the
  shared EQ, the `spatcore::io` device layer (`io/`, target `spatcore-io`),
  the shared patch matrix (`ui/patch/`), and the `binaural/` module —
  head-orientation sources, head-tracker plugin C ABI, SOFA→HrirDatabase
  pipeline; header-only, plus the `spatcore-mysofa` target). rt/dsp/wfs/
  reverb/gpu + control (osc/state/mcp) + controllers + ui + io + binaural.
  Adopting the io layer and patch window in XOA:
  see `Documentation/XOA-AUDIO-DEVICE-AND-PATCH-HANDOFF.md`. Binaural
  monitoring (WP15) is **implemented** — `Documentation/XOA-DEVPLAN.md` §WP15
  is authoritative; `Documentation/hoa-binaural-handoff.md` is the original
  handoff kept for intent, with its corrections listed at the top.
  The CMake wiring comes from
  spatcore's `cmake/SpatcoreConsumer.cmake` helper (see CMakeLists.txt).
  Dependency direction is strictly app → spatcore; never modify spatcore from
  here — changes go to the spatcore repo and arrive via a pin bump.
- `ThirdParty/JUCE` — submodule (JUCE 9.0.1, tag `9.0.1`; same major as
  WFS-DIY).
- `ThirdParty/hidapi` — submodule (headers for spatcore-controllers; static
  lib linked into the app via hidapi's own CMake).
- `ThirdParty/juce_simpleweb`, `ThirdParty/roli_blocks_basics` — vendored JUCE
  modules (copied from WFS-DIY), required by spatcore-control / -controllers.
- `ThirdParty/libmysofa`, `ThirdParty/zlib` — vendored trimmed trees (copied
  from WFS-DIY), compiled by spatcore's `spatcore-mysofa` target for the SOFA
  loader (binaural monitoring, WP15).
- `assets/SOFA/` — the bundled SADIE II KU100 HRTF set (default binaural
  monitoring set; also the SOFA smoke-test fixture).

## Conventions

- C++17; compile flags come from `spatcore_apply_compile_flags()` (pins the
  optimization/FP flags spatcore's bit-exactness gates were baselined with —
  `/fp:precise`, `/Ox` Release, LTO on MSVC).
- Ambisonics: order 10, **ACN** channel ordering, **SN3D** normalization
  (AmbiX) → 121 SH channels. Constants in `Source/XoaConstants.h`.
- Two frames meet in the binaural path and run in OPPOSITE azimuth senses:
  XOA's soundfield frame is +X front / **+Y left**, spatcore's HRIR grid and
  head attitude are SOFA-style (**azimuth positive to the listener's right**).
  Every conversion lives in `Source/DSP/AmbiHeadMapping.h` and
  `AmbiBinauralDecoder.h`'s `gridAzToXoaAzDeg` — never inline at a call site
  (`AmbiRotation.h` states the same policy for the DSP).
- License GPLv3. Third-party inventory in `THIRD_PARTY_NOTICES.md`.

## Where things are decided

- Roadmap and architecture decisions: `Documentation/XOA-PLAN.md`.
- Execution order and decision records (numeric D1-D54, plus the named WP8 ones
  such as `D-NFCstage` / `D-stems`; D55 is the next free number):
  `Documentation/XOA-DEVPLAN.md` (it wins over the PRD where they conflict).
  Requirements: `Documentation/XOA_PRD.md`.
  Frozen OSC contract: `Documentation/XOA-OSC-MAP.md`.
- Migrating the audio device layer onto `spatcore::io`, and later adopting the
  Audio Interface window and patch matrix lifted from WFS-DIY:
  `Documentation/XOA-AUDIO-DEVICE-AND-PATCH-HANDOFF.md`. Read it before
  touching `Source/Audio/AudioEngine.{h,cpp}` — the device layer is available
  on spatcore main now, and the migration has invariants (buffer rows that are
  simultaneously an input and an output; buffer width is not the speaker count)
  that silently corrupt audio if missed.
- The renderer/engine seams XOA plugs into (algorithm method contract,
  `RtSnapshot`, raw-pointer matrix hand-off): spatcore docs
  (`spatcore/docs/*.md`) + `Documentation/XOA-PLAN.md` §2.
- Reference app for porting shell pieces (parameter store pattern, OSC/MCP
  managers, GUI framework): the WFS-DIY checkout at `d:/dev/WFS_DIY_v1`.
