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
- `spatcore/` — **submodule**, pinned to **8296f28** (post-v0.1.1: GPU
  node-parallel SDN, Max-port FR diffusion, MCP protocol negotiation, the
  shared EQ). rt/dsp/wfs/reverb/gpu +
  control (osc/state/mcp) + controllers + ui. The audio device layer (`io/`,
  target `spatcore-io`) landed later, at `8e0d7e6`, and is **not** in this pin —
  see `Documentation/XOA-AUDIO-DEVICE-AND-PATCH-HANDOFF.md`. The CMake wiring comes from
  spatcore's `cmake/SpatcoreConsumer.cmake` helper (see CMakeLists.txt).
  Dependency direction is strictly app → spatcore; never modify spatcore from
  here — changes go to the spatcore repo and arrive via a pin bump.
- `ThirdParty/JUCE` — submodule (JUCE 9.0.0, tag `9.0.0`; same major as
  WFS-DIY).
- `ThirdParty/hidapi` — submodule (headers for spatcore-controllers; static
  lib linked into the app via hidapi's own CMake).
- `ThirdParty/juce_simpleweb`, `ThirdParty/roli_blocks_basics` — vendored JUCE
  modules (copied from WFS-DIY), required by spatcore-control / -controllers.

## Conventions

- C++17; compile flags come from `spatcore_apply_compile_flags()` (pins the
  optimization/FP flags spatcore's bit-exactness gates were baselined with —
  `/fp:precise`, `/Ox` Release, LTO on MSVC).
- Ambisonics: order 10, **ACN** channel ordering, **SN3D** normalization
  (AmbiX) → 121 SH channels. Constants in `Source/XoaConstants.h`.
- License GPLv3. Third-party inventory in `THIRD_PARTY_NOTICES.md`.

## Where things are decided

- Roadmap and architecture decisions: `Documentation/XOA-PLAN.md`.
- Execution order and decision records (numeric D1-D34, plus the named WP8 ones
  such as `D-NFCstage` / `D-stems`; D35 is the next free number):
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
