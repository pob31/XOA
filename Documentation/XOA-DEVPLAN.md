# XOA — Detailed Development Plan

Version 0.1 — July 2026 — GPL-3.0

This document turns the roadmap into an executable sequence of work packages.
The three planning documents split authority like this:

- **[XOA_PRD.md](XOA_PRD.md)** — *what* to build: requirements FR-1…FR-24,
  performance targets (§7), risks (§9), acceptance criteria (§10),
  milestones M0–M5.
- **[XOA-PLAN.md](XOA-PLAN.md)** — *architecture*: fixed conventions, the
  spatcore seams XOA plugs into, phases P0–P8.
- **XOA-DEVPLAN.md** (this file) — *execution order*: work packages WP0–WP14,
  dependencies, tests, exit criteria. Where ordering conflicts with the PRD's
  milestone sequence, this document wins and records the deviation. There is
  exactly one: **GPU decode (M4) is scheduled after the GUI port** (see WP11).

---

## 1. Status and mapping

**Milestone M1 reached** (WP0–WP6 done). The repo is a CMake-native spatcore
v0.1.1 consumer with the full CPU Ambisonics chain: the parameter store +
XML project I/O (WP2), the SH math core (WP3), SO(3) rotation + mirror (WP4),
the SAD/mode-matching decoder designer + rV/rE (WP5), and the RT bus engine —
gather (convention + FR-7 order adaptation) → click-free SO(3) rotation →
decode GEMM → master gain → device outs — with multichannel file playback, a
synthetic order-10 test-scene generator, an offline-render bit-exact harness,
and a throwaway shell UI (WP6). The `xoa-tests` suite and the
`xoa-offline-render-smoke` run on all three CI OSes.

M1 "first audible" evidence: the offline-render harness renders the exact
chain deterministically (per-machine SHA baselines) and to a playable
24-channel WAV; the WP6 B2 null test pins `processBlock` output to
`matrix · scene` within float tolerance; the app launches, opens a device,
rebuilds the decoder, and runs. The remaining M1 check — listening on a real
rig / loopback across 44.1/48/96 kHz — is a developer step (no >64-out
hardware in CI).

| WP | Title | Milestone | Phase | Depends on | Size | Status |
|---|---|---|---|---|---|---|
| WP0 | Bootstrap | M0 | P0 | — | — | **DONE** |
| WP1 | Test scaffolding & CI step 1 | pre-M1 | — | WP0 | S | **DONE** |
| WP2 | Parameter store, schema, project I/O (XML) | M1 part | P1 | WP1 | L | **DONE** |
| WP3 | SH math core: evaluation, conventions, order weights | M1 part | P2 | WP1 | M | **DONE** |
| WP4 | Rotation & mirror | M1 part | P2 | WP3 | M | **DONE** |
| WP5 | Speaker layout & decoder designer v1 (SAD + mode-matching, rV/rE) | M1 part | P2 | WP2, WP3 | L | **DONE** |
| WP6 | RT bus engine, file playback, minimal shell — **first audible** | **M1 exit** | P2 + P5 sliver | WP2, WP4, WP5 | XL | **DONE (M1)** |
| WP7 | AllRAD, dual-band, per-speaker comp, test signals | **M2 exit** | P2 tail | WP6 | XL | next |
| WP8 | Mono encoders, NFC, spread | M3 part | P2 tail | WP6 | L | |
| WP9 | OSC & head-tracking (generic quaternion) | **M3 exit** | P3 (scoped) | WP2, WP4, WP8 | M | |
| WP10 | GUI framework port, XOA tabs, rV/rE visualization | M5 viz part | P5 | WP6, WP9 | XL | |
| WP11 | GPU decode path (two tracks, cross-repo) | **M4 exit** | P7 | WP7 (+ spatcore track) | XL | |
| WP12 | MCP server + AI undo | M5 part | P4 | WP2, WP9, WP10 (UI bits) | M | |
| WP13 | Acceptance, hardening, performance — **v1.0** | **M5 exit** | — | all | L | |
| WP14 | Ship: installers, release CI, docs | — | P8 | WP13 | M | |

Parked to post-v1 (see §8): binaural monitoring (was P6), zoom/focus warping
(FR-11 v1.1), PSN/RTTrP/MQTT tracker profiles + OSCQuery (P3 tail),
SH-domain reverb, A→B conversion, `spatcore/ambi/` extraction.

---

## 2. Dependency graph and parallel tracks

```
WP1 ──→ (everything)

WP2 ─────────────┬──→ WP5 ──→ WP6 ──→ WP7 ──→ WP11 (XOA track)
WP3 ──→ WP4 ─────┘             │  └──→ WP8 ──→ WP9 ──→ WP12
                               │                 │
                               └── shell UI ─────┴──→ WP10 ──→ WP13 ──→ WP14
                                                        ↑
   spatcore IAmbiBackend track: design after WP6 ───────┴──→ WP11
```

Four tracks, for the day more than one pair of hands is available:

- **Track A — DSP math**: WP3 → WP4 → WP5. Pure, non-RT, test-driven; zero
  dependency on WP2 and fully interleavable with it.
- **Track B — control**: WP2 → WP9 → WP12.
- **Track C — GUI**: minimal shell inside WP6/WP7, full kit port at WP10.
- **Track D — spatcore GPU** (separate repo): `IAmbiBackend` design can start
  once WP6 freezes the matrix contracts; kernel/plugin work proceeds during
  WP9/WP10; lands in XOA as WP11 via pin bump.

Single-developer reading: the WP numbers *are* the schedule, in order.

---

## 3. Cross-cutting engineering rules

These hold for every work package.

- **spatcore no-touch rule** (CLAUDE.md): never modify `spatcore/` from this
  repo; changes go to the spatcore repo and arrive via a pin bump.
  Consequence: all Ambisonics DSP is prototyped **app-local** in
  `Source/DSP/Ambi*`. Extraction to `spatcore/ambi/` is a separate post-v1
  spatcore-repo project, gated on bit-exact baselines (the same lifecycle
  that produced spatcore from WFS-DIY).
- **RT rules** (spatcore discipline): no `ValueTree` reads on RT threads;
  POD hand-off via `spatcore/rt/RtSnapshot.h`; live flat-matrix seam —
  app-owned `std::vector<float>` matrices handed as `const float*` at
  `prepare()`, rewritten in place by a message-thread timer, single-writer
  invariant, benign staleness; click-free changes via
  `spatcore/dsp/DelayTargetSmoother.h` or per-coefficient linear ramps
  (≤ one buffer).
- **Order-generic rule (FR-3)**: no per-order hardcoded *logic*; every size
  derives from `xoa::kAmbisonicOrder` / `xoa::kNumSHChannels` in
  `Source/XoaConstants.h`. Numeric *data* tables (reverse-Bessel roots,
  t-designs, max-rE roots) are permitted constants, provided the generator
  script that produced them is committed under `tools/reference/`.
- **Test conventions**: dependency-free CHECK-macro console app, cloning the
  `spatcore/tests/SpatcoreTests.cpp` pattern — **no gtest/Catch2** (keeps the
  repo's no-new-dependencies posture and matches spatcore). Golden data lives
  in `tests/data/*.json`, generated by committed Python scripts in
  `tools/reference/` (scipy/numpy, versions pinned in the script header).
  Design-side math runs in **double** precision; **float** only at the RT
  apply boundary.
- **File layout**: `Source/DSP/Ambi*` (Ambisonics DSP), `Source/Parameters/`
  (store/schema/file I/O), `Source/Audio/` (engine, player, test signals),
  `Source/Network/` (OSC, MCP), `Source/gui/`, `ThirdParty/` (vendored
  single-header libs), `tools/validation/` (mirrors WFS-DIY's harness
  layout), `tools/reference/` (golden-data generators).

---

## 4. Decisions

Recorded here so no work package has to re-litigate them.

- **D1 — Project files are XML via `spatcore/control/state/XmlPersistence`.**
  This diverges from FR-24's literal wording ("JSON, human-diffable") and is
  deliberate: XmlPersistence brings rolling timestamped backups,
  merge-backfill with an injected validator, and the section-split file
  pattern for free, and it makes the FR-16 WFS-DIY layout import near-trivial
  (WFS-DIY project sections are XML). XML satisfies the *intent*
  (human-diffable, versionable); FR-18's CSV/JSON matrix export covers
  interchange, and a JSON project-export utility sits in the post-v1 backlog
  (§8) if demand appears.
- **D2 — Binaural monitoring is post-v1** (PRD already lists it as a v1
  non-goal beyond a basic utility). The head-orientation `RtSnapshot` seam it
  needs is built anyway in WP9, so deferral costs no architecture. An
  optional S-sized "utility monitor decode" (static virtual-speaker stereo
  downmix, no HRTF/SOFA) may be taken as a stretch inside WP13.
- **D3 — PSN/RTTrP/MQTT tracker profiles and OSCQuery are post-v1.**
  FR-10 requires only a generic OSC quaternion for v1 ("specific tracker
  profiles later"). This scopes WP9 down from XOA-PLAN's fuller P3.
- **D4 — Decoder set for v1 is resolved by the PRD**: SAD *and*
  mode-matching (FR-12) *and* AllRAD (FR-13). XOA-PLAN §5's open question is
  closed.
- **D5 — AllRAD geometry: vendor, don't implement.** Vendor `convhull_3d`
  (single-header 3-D quickhull, MIT license, GPL-3.0-compatible) into
  `ThirdParty/convhull_3d/` with an entry in `THIRD_PARTY_NOTICES.md`, plus
  committed t-design coordinate tables with provenance notes. Robust
  quickhull (degeneracy handling) is weeks of avoidable work.
- **D6 — GUI before GPU** (the one deviation from PRD milestone order,
  M4 ↔ M5-viz). Rationale in WP11: the GPU family is spatcore-repo work that
  must not start before the matrix contracts freeze (post-WP7), and it runs
  as a parallel track anyway; rV/rE visualization needs the WP10 GUI kit;
  both land before WP13 acceptance either way.

---

## 5. Work packages

Template per package: **Goal · PRD coverage · Tasks · Port sources ·
spatcore assets · DSP/math notes · Tests & exit criteria · Risks · Size.**

---

### WP1 — Test scaffolding & CI evolution step 1 (S)

**Goal.** A failing test can turn CI red before the first line of DSP exists.
XOA-PLAN P2 demands "offline render baselines from day one"; this package is
the day-zero half of that discipline.

**PRD coverage.** None directly; prerequisite for all FR validation.

**Tasks.**
- `tests/CMakeLists.txt` + `tests/XoaTests.cpp`: `juce_add_console_app(xoa-tests)`
  linking `spatcore-audio`/`spatcore-control`, `spatcore_apply_compile_flags`,
  `JUCE_WEB_BROWSER=0` / `JUCE_USE_CURL=0` — a direct clone of the
  `spatcore/tests/` pattern (plain CHECK macro, exit 0 = pass).
- Root `CMakeLists.txt`: `enable_testing()` + `add_test`.
- CI (`.github/workflows/ci.yml`): add a test step to all three OS jobs.
  Multi-config generators need `ctest -C Debug`.
- `tools/reference/README.md`: the golden-data convention (committed JSON in
  `tests/data/`, generated by committed pinned-scipy scripts, provenance
  documented per file).
- Seed test: `xoa::kNumSHChannels == (xoa::kAmbisonicOrder + 1)²` and a
  trivial spatcore-symbol smoke check.

**Exit.** `ctest` runs `xoa-tests` locally and on all three CI OSes; a
deliberately broken CHECK turns CI red.

**Risks.** None material.

---

### WP2 — Parameter store, schema, project I/O (L)

**Goal.** The single source of truth for every parameter, with persistence.
Everything downstream (OSC, MCP, GUI, engine snapshots) hangs off this schema.

**PRD coverage.** FR-24 (project files, per D1), FR-16 schema side (layout
lives in the Speakers section), groundwork for FR-22/FR-23.

**Tasks.**
- `Source/Parameters/XoaValueTreeState.{h,cpp}` deriving
  `spatcore::control::state::TreeParameterStore`: implement
  `getTreeForParameter()`, `handlePostWrite` invariants, per-domain
  `UndoManager`s (proposed domains: Config, Inputs, Speakers, Decoder,
  Monitoring).
- Schema tables as C++ headers, following the WFS-DIY shape:
  `Source/Parameters/XoaParameterIDs.h`, `XoaParameterDefaults.h`,
  `XoaConstraints.h`. Sections per XOA-PLAN P1: **Config**
  (show/IO/stage/master/network), **Inputs** (position in
  cart/cyl/spherical, gain, width/spread, NFC on/off), **Speakers** (layout
  geometry, per-output trim/delay/EQ), **Decoder** (type, dual-band, max-rE,
  crossover), **Monitoring** (reserved).
- Port verbatim from WFS-DIY: `Source/Helpers/CoordinateConverter.h`
  (cart/cyl/spherical; **app-side code — spatcore does not have it**) and
  `Source/Parameters/ParameterDirtyTracker.h`.
- `Source/Parameters/XoaFileManager.{h,cpp}` composing
  `spatcore/control/state/XmlPersistence`: section-split files
  (show/system/inputs/speakers/decoder/network), a small `.xoa` project
  manifest, snapshots with scope filtering, rolling backups. **Schema-version
  field from day one** so merge-backfill has something to key on.

**Port sources.** `d:/dev/WFS_DIY_v1/Source/Parameters/WFSValueTreeState.{h,cpp}`,
`WFSParameterIDs.h`, `WFSParameterDefaults.h`, `WFSConstraints.h`,
`WFSFileManager.{h,cpp}`; `d:/dev/WFS_DIY_v1/Source/Helpers/CoordinateConverter.h`.

**spatcore assets.** `TreeParameterStore`, `XmlPersistence`,
`OscTransportTypes.h` (`OriginTag` + origin-aware undo suppression).

**Tests & exit.** Save/load round-trip is bit-stable; merge-backfill of a
file written with a reduced schema gains the new defaults; constraint and
coordinate-conversion unit tests; a WFS-DIY project's speaker section
*parses* (full import semantics land in WP5).

**Risks.** Schema churn as later WPs discover missing parameters — mitigated
by the version field + backfill from the start. Order the internal tasks so
store + schema land before file I/O.

---

### WP3 — SH math core: evaluation, conventions, order weights (M)

**Goal.** Correct real spherical harmonics and convention conversions —
the foundation everything else multiplies by. A silent sign or scaling error
here poisons every downstream package, so this WP is test-heavy.

**PRD coverage.** FR-1, FR-2, FR-3, FR-7 (the weight/adaptation math;
runtime wiring in WP6).

**Tasks.**
- `Source/DSP/AmbiSphericalHarmonics.h` — real SH evaluation, ACN indexing,
  SN3D normalization, **no Condon–Shortley phase** (AmbiX convention).
- `Source/DSP/AmbiConventions.h` — N3D↔SN3D diagonal scaling; FuMa→ACN/SN3D
  for orders ≤ 3 (legacy import, UI-flagged); **explicit rejection with a
  clear message for FuMa > 3** (PRD §9).
- `Source/DSP/AmbiOrderWeights.h` — max-rE weights per order (exact, see
  notes), basic/in-phase weight families, order up/down adaptation (FR-7:
  zero-pad up + optional gentle order-weighting shelf; truncate down with
  target-order max-rE re-weighting).

**DSP/math notes.**
- Associated Legendre via the **Schmidt semi-normalized two-term upward
  recurrence** (seed the sectoral term P̄ₘₘ with incremental √-factors, then
  recurse (ℓ−1,m),(ℓ−2,m) → (ℓ,m)). SN3D *is* Schmidt semi-normalization, so
  no post-conversion step exists to get wrong, and the recurrence stays
  stable far beyond n=10 — the right habit for order-generic code.
- Azimuth terms by direct `sincos(mφ)`; 121 evaluations don't justify a
  Chebyshev recurrence.
- Max-rE exactly: g_ℓ = P_ℓ(r_E) where r_E is the largest root of P_{N+1},
  Newton-refined from the Zotter–Frank seed cos(137.9°/(N+1.51)); keep the
  closed-form approximation as a cross-check in tests.

**Tests & exit.**
- Golden values from `tools/reference/gen_sh_reference.py` (scipy
  `sph_harm` → real SH with the Condon–Shortley/real-basis conversion
  **documented inside the script** — this conversion is the classic silent
  error, so it gets written down once, next to the code that does it).
- Orthonormality quadrature over a committed t-design grid:
  Σᵢ wᵢ Ȳₙ(dᵢ) Ȳₘ(dᵢ) ≈ δₙₘ under N3D scaling, tolerance 1e-10 (double).
- Pole/axis symmetry property tests; FuMa round-trip order ≤ 3; max-rE
  weights vs published order 1–7 tables.

**Risks.** Convention bugs are inaudible in isolation and catastrophic
downstream. Mitigation: two independent references (scipy goldens + published
AmbiX/IEM tables) must agree before WP4 starts.

---

### WP4 — Rotation & mirror (M)

**Goal.** Full SO(3) soundfield rotation and the three mirror planes, as
pure matrix machinery (RT application lands in WP6).

**PRD coverage.** FR-9; FR-11 mirror half (zoom/focus parked, §8).

**Tasks.**
- `Source/DSP/AmbiRotation.{h,cpp}` — recursive per-order rotation matrices
  (**Ivanic–Ruedenberg**), block-diagonal per order ℓ ((2ℓ+1)×(2ℓ+1));
  built from the ℓ=1 rotation. Inputs: quaternion and yaw/pitch/roll.
- `Source/DSP/AmbiMirror.h` — sign-flip tables: left/right = negate m<0
  terms; front/back = (−1)^m on cos terms, (−1)^{m+1} on sin terms;
  up/down = (−1)^{ℓ+m}.
- Click-free swap policy (consumed by WP6): linearly interpolate old→new
  full rotation matrix over one block. A lerped matrix is momentarily not
  exactly SO(3); over one block this is inaudible and costs a single apply —
  cheaper than a dual-apply crossfade.

**DSP/math notes.** Implement against **Ivanic & Ruedenberg (1996) *with the
1998 erratum*** — the uncorrected coefficient tables are the classic
implementation trap; the code comments must cite the erratum. Mind the ℓ=1
basis permutation: ACN order 1 channels are (Y, Z, X), not Cartesian
(x, y, z). Cost check: Σₗ (2ℓ+1)² = 1771 MACs/sample at order 10 — CPU is
fine (PRD §5).

**Tests & exit.**
- Property test (the decisive one): `rotate_field(R, encode(dir)) ==
  encode(R·dir)` over random orientations and directions, all orders ≤ 10,
  tolerance 1e-12 in double. This catches erratum and basis-order mistakes
  in one shot.
- Golden Wigner-D/rotation matrices from a sympy/scipy generator in
  `tests/data/rotation_reference.json`.
- Composition: R(q₁)·R(q₂) == R(q₁∘q₂); mirror involution M² = I; mirror ∘
  encode property tests.

**Risks.** The 1998 erratum; ℓ=1 permutation. Both are covered by the
property test, which is written *first*.

---

### WP5 — Speaker layout & decoder designer v1 (L)

**Goal.** Non-RT decoder design proven against golden references *before*
any audio plumbing exists, so WP6 debugs only RT code, never math. Covers
SAD and mode-matching (AllRAD follows in WP7) plus the rV/rE analysis data
that makes decoders inspectable (FR-18).

**PRD coverage.** FR-12, FR-16, FR-17, FR-18 (data/export half).

**Tasks.**
- Layout model: populate the WP2 Speakers schema section — positions in
  meters, groups, per-speaker metadata. **Note: spatcore has no layout
  model; positions live in the app ValueTree, as in WFS-DIY.**
- WFS-DIY project import (FR-16): map WFS-DIY's `outputs.xml` speaker
  section into the XOA Speakers section via `XoaFileManager`.
- Regularity detection: classify a layout (ring / dome / sphere-ish /
  irregular) and *suggest* SAD/mode-matching vs AllRAD — never auto-select
  (FR-16: "detected and suggested, never assumed").
- `Source/DSP/AmbiDecoderDesigner.{h,cpp}`:
  - **SAD**: D = (4π/L)·Ỹᵀ on N3D-rescaled SH at speaker directions;
    basic / max-rE per-order diagonal weighting; amplitude vs energy
    normalization options.
  - **Mode-matching**: SVD pseudo-inverse; report condition number κ and
    warn above a threshold (FR-17); optional Tikhonov regularization.
- `Source/DSP/AmbiRvReAnalysis.h` — rV/rE vectors and energy over a
  committed sphere grid; export decode matrix + analysis as CSV/JSON (FR-18).
- Non-RT `DecoderMatrixBuilder` worker thread + atomic hot-swap plumbing
  (double-buffered matrix / `RtSnapshot`-published pointer swap; consumed by
  WP6; FR-17).
- **Define the concrete rV/rE tolerances used by acceptance (§7 of this doc,
  PRD §10)** — proposed defaults, adjustable after first measurements:
  rE direction error < 5° within the rig's coverage region above crossover;
  ‖rV‖ within ±0.05 of 1 below crossover on regular rigs.

**Port sources.** Layout-geometry inspiration only:
`d:/dev/WFS_DIY_v1/Source/Helpers/ArrayGeometryCalculator.*`,
`Source/gui/OutputArrayHelperWindow.*` (editor UI ports at WP10).

**spatcore assets.** `rt/RtSnapshot.h` for the swap; JUCE linear algebra is
insufficient — small dense SVD implemented locally in double (121×L is tiny)
or via a vendored single-header, decided at implementation time.

**Tests & exit.** SAD ≈ mode-matching on a t-design layout to tight
tolerance (they coincide on perfectly regular layouts); golden decode
matrices for two committed fixtures (24-speaker ring, hemispherical dome)
generated by an octave/IEM-toolbox reference with **provenance documented**;
κ warning fires on a deliberately pathological layout; rV/rE on the ring
matches analytic expectation; matrix CSV/JSON export round-trips.

**Risks.** Reference-matrix provenance (document generation exactly);
ill-conditioned layouts (that's what κ reporting is for).

---

### WP6 — RT bus engine, file playback, minimal shell — **M1, first audible** (XL)

**Goal.** The PRD's first audible milestone: an AmbiX file of any order 1–10
plays through order adaptation → rotation → SAD decode to a real rig,
CPU-only, click-free — plus the offline-render harness that gates everything
after it.

**PRD coverage.** FR-4, FR-7 (runtime), FR-8, FR-9 (RT apply), FR-10 (UI
dials), FR-19/FR-20 (architecture check), M1 exit. PRD §9's "order-10
content is rare" mitigation (test-scene generator) lands here.

**Tasks.**
- `Source/Audio/AudioEngine.{h,cpp}` — extract audio-device hosting and the
  callback out of `MainComponent` (device layer is JUCE/app-owned; spatcore
  deliberately does not provide it).
- `Source/DSP/AmbiBusAlgorithm.h` (+ `AmbiBusProcessor.h` if fork-join
  workers are warranted) — implements the spatcore duck-typed algorithm
  contract (`prepare(numIn, numOut, sampleRate, blockSize, const float*
  matrices…, enabled)` / `processBlock` / `setProcessingEnabled` /
  `releaseResources` / metering accessors), modeled on
  `spatcore/wfs/InputBufferAlgorithm.h` + `InputBufferProcessor.h`.
  Chain: input → boundary conversion (FR-2, WP3) → order adapt (FR-7, WP3)
  → rotation apply (WP4 matrices via `RtSnapshot`, one-block lerp) →
  decode GEMM (`[121 × out]` matrix from WP5, hot-swapped) → outs.
- `Source/DSP/AmbiRtTypes.h` — trivially-copyable POD snapshots (rotation
  state, decoder swap handle), modeled on WFS-DIY
  `Source/DSP/BinauralCalculationEngine.h::RtParams`.
- `Source/Audio/FilePlayer.{h,cpp}` — multichannel WAV/CAF/FLAC up to
  128 ch, AmbiX metadata detection where present, manual order/convention
  override always available (FR-8).
- **Spike (early, time-boxed):** verify JUCE `WavAudioFormat`/CAF actually
  reads 121–128-channel files on all three OSes before building on it.
- Minimal shell UI in `MainComponent` (plain JUCE, no LookAndFeel port —
  deliberately throwaway): transport + file open, yaw/pitch/roll dials,
  layout/decoder pick, output meters, measured-latency readout (PRD §7
  "report actual in UI").
- **Offline-render harness**: port the WFS-DIY pattern to
  `tools/validation/offline-render/` (`main.cpp`, `scenarios.h`,
  `sha256.h`, `--check baselines/<machine>.json`, `--update`, `--bench`) —
  a `juce_add_console_app` compiling the app's DSP headers. XOA scenarios:
  static 3rd-order scene, moving rotation, order-adapt up/down toggle.
- **Synthetic test-scene generator** (order-10 encoded moving sources,
  from WP3 math) — both an offline-render scenario and a UI-triggerable
  source for rig validation without external order-10 material.
- First committed baselines; CI builds the harness and runs render smoke
  (baseline `--check` stays per-machine at this stage — see §6).

**Port sources.** `d:/dev/WFS_DIY_v1/tools/validation/offline-render/`
(main/scenarios/sha256/baselines pattern);
`Source/DSP/BinauralCalculationEngine.h` (RtParams pattern).

**spatcore assets.** `wfs/InputBufferAlgorithm.h` contract template,
`rt/RtSnapshot.h`, `dsp/DelayTargetSmoother.h`, `rt/RealtimeThreadUtil.h`,
`dsp/NumericGuards.h`.

**Tests & exit — M1 acceptance.** A 3rd-order AmbiX file plays and rotates
click-free (rotation parameter → audio ≤ 2 buffers, PRD §7), SAD-decoded to
the 24-ring fixture, CPU only, at 44.1–96 kHz; offline-render null-test:
decode of a known scene equals matrix·scene to float tolerance (matrices
from WP5 goldens); baselines committed; CI green with the harness building
on all three OSes.

**Risks.** >64-channel file I/O in JUCE (the spike); no 121-out hardware on
dev machines (harness + virtual/loopback devices mitigate); cross-OS FP
drift in baselines (`spatcore_apply_compile_flags` exists exactly for this —
keep per-machine baselines like WFS-DIY until §6 promotes them).

---

### WP7 — AllRAD, dual-band, per-speaker compensation, test signals — **M2** (XL)

**Goal.** "The shoebox works": irregular rigs decode honestly, the decoder
splits into velocity/energy bands, and per-speaker alignment makes real
rooms usable.

**PRD coverage.** FR-13, FR-14, FR-15, FR-21; §7 decoder-rebuild target.

**Tasks.**
- Vendor `ThirdParty/convhull_3d/` (D5) + `THIRD_PARTY_NOTICES.md` entry.
- `Source/DSP/TDesignTables.h` — committed virtual-layout design (dense,
  ~500–1000 points per AllRAD practice; must be ≥ t=21-capable for N=10)
  with provenance notes and the generator/download script in
  `tools/reference/`.
- `Source/DSP/AmbiVBAP.h` — triangle-wise 3×3 inversion + gain
  normalization.
- `Source/DSP/AmbiAllRAD.{h,cpp}` — decode to the virtual t-design, VBAP
  virtual sources to the real rig; **imaginary loudspeakers** auto-inserted
  at coverage gaps (e.g. missing floor), their gains discarded after
  triangulation; energy normalization across the rig.
- **Dual-band (FR-14)** as a **diagonal-weight factorization with a single
  decode GEMM**: phase-matched LR4 (squared-Butterworth) split of the
  121-ch bus, basic weights on LF / max-rE on HF (+ per-band level-match
  scalar), recombine, decode once. Valid because basic and max-rE differ
  only by per-order diagonal weights for all three decoder types. The PRD
  §5 two-GEMM formulation is documented in code comments as the GPU-friendly
  alternative for WP11. Crossover default 400 Hz, user-adjustable,
  rig-radius-scaled suggestion.
- **Per-speaker compensation (FR-15)** via spatcore:
  `wfs/OutputBufferAlgorithm.h` + `OutputBufferProcessor.h` (per-output
  delay + gain + 6-band `dsp/OutputEQBiquadFilter`), driven from the WP2
  Speakers schema; distance-delay alignment and 1/r (or user-law) distance
  gain computed in the calculation engine; trim/mute/solo.
- `Source/Audio/TestSignalGenerator.h` — **new code** (FR-21: pink-noise
  burst, sweep, speaker-identification mode; the PRD's "reuse from spatcore
  where present" is an empty set — nothing exists there).
- Shell UI additions: per-speaker trim/mute/solo table, test-signal panel.
- Offline-render scenarios extended (shoebox fixture, dual-band on/off);
  baselines updated.

**DSP/math notes.** AllRAD per Zotter–Frank. Hull degeneracy is a
first-class case, not an edge case: a flat ring is degenerate for a 3-D
hull — the AllRAD path must auto-insert imaginary top/bottom speakers (or
explicitly fall back to SAD with a UI notice) and this behavior is tested.

**Tests & exit — M2 acceptance.** The irregular-shoebox fixture decodes via
AllRAD with sensible rV/rE; hemispherical dome with auto imaginary floor
speaker; decoder rebuild (order 10, 250 spk) ≤ 2 s and non-blocking (§7);
dual-band nulls against single-band when both bands use identical weights;
coplanar-ring degeneracy test passes; per-speaker delay/gain verified by
offline render.

**Risks.** PRD §9: AllRAD quality on severe shoeboxes — schedule a
**listening-validation checkpoint** on a real irregular rig before calling
M2 done, and note **user-placeable imaginary speakers** as the planned
fallback feature (post-v1 backlog if not needed sooner). Hull robustness on
colinear/coplanar input (covered by tests + convhull_3d).

---

### WP8 — Mono encoders, NFC, spread (L)

**Goal.** The theatre-integrator scenario: N mono stems encoded onto the
bus with position, distance, and width — including stable order-10
near-field compensation.

**PRD coverage.** FR-5, FR-6; M3 first half.

**Tasks.**
- Encoder path in `AmbiBusAlgorithm` via the live `[in × 121]` matrix seam
  (XOA-PLAN §2.2a): app-owned flat matrix, message-thread timer rewrites,
  workers read live; per-coefficient linear ramps ≤ one buffer (FR-5).
- `Source/DSP/AmbiCalculationEngine.{h,cpp}` (control side): positions →
  encode coefficients (WP3 SH evaluation × gain), distance gain law,
  spread.
- Spread (FR-5) as an energy-normalized **order taper**: max-rE-style
  per-order weights parameterized by spread angle (Kronlachner/Zotter
  widening).
- `Source/DSP/AmbiNFCFilter.h` (FR-6): NFC transfer H_ℓ(s) =
  F_ℓ(s·r_ref/c)/F_ℓ(s·r_src/c) realized as **cascaded first/second-order
  sections from tabulated reverse-Bessel-polynomial roots, ℓ = 1…10**
  (roots computed offline by a committed scipy script in
  `tools/reference/`, stored as constants), bilinear-transformed at the
  active sample rate (FR-4); minimum-radius clamp and gain ceiling for
  numerical safety.
- Incoming position conditioning: `dsp/TrackingPositionFilter.h` (1-Euro) +
  `dsp/InputSpeedLimiter.h`.
- **First task of the package, not last** (PRD §9 says validate early):
  NFC stability + magnitude-response validation at order 10 / 44.1 kHz.

**spatcore assets.** `TrackingPositionFilter`, `InputSpeedLimiter`,
`DelayTargetSmoother`-style ramps, the matrix seam.

**Tests & exit — M3(a).** N mono stems encode, position, and move
click-free; NFC magnitude responses match Daniel reference curves (scipy
goldens) for ℓ = 1…10 at 44.1/48/96 kHz across a radius sweep, within
tolerance, with stability asserted at the worst case (order 10, 44.1 kHz,
small radius → clamped); CPU headroom measured against PRD §7 (< 40 % on the
reference shape); offline-render encoder scenarios + baselines.

**Risks.** PRD §9: NFC numerics near Nyquist and small radii — mitigated by
root tabulation (not naive transfer functions), clamps, and early
validation. Encode-matrix churn rate vs ramp smoothness under fast source
movement (measure; 1-Euro conditioning helps).

---

### WP9 — OSC & head-tracking — **M3 exit** (M)

**Goal.** Every runtime parameter drivable over OSC; a generic quaternion
head-tracker rotates the field.

**PRD coverage.** FR-22, FR-10 (OSC + tracker sources); M3 exit.

**Tasks.**
- **Freeze the address map first**: `Documentation/XOA-OSC-MAP.md` —
  `/xoa/...` scheme consistent with WFS-DIY conventions (per-family
  OSCQuery-style and standard forms), written before code.
- `Source/Network/OSCManager.{h,cpp}` — a **scoped rewrite borrowing
  structure** from WFS-DIY's `OSCManager`/`OSCMessageRouter` (the 227 KB
  original is WFS-shaped; port the architecture — targets, rate limiting,
  IP filtering, router — not the file).
- Wire spatcore transports: `OSCReceiverWithSenderIP`, `OSCTCPReceiver`,
  `OSCParser`/`OSCSerializer`, `OSCIngestQueue`, `OSCRateLimiter` (50 Hz
  coalesce), `OriginTagScope` for origin attribution.
- Head-tracking: generic OSC quaternion → `TrackingIngestQueue` → rotation
  `RtSnapshot` (D3: no PSN/RTTrP/MQTT profiles, no OSCQuery in v1).
- OSC out: parameter feedback + meter/state streams.
- Port `tools/validation/control-replay/osc_replay.py` with XOA fixtures.

**Port sources.** `d:/dev/WFS_DIY_v1/Source/Network/OSCManager.*`,
`OSCMessageRouter.*` (structure), `tools/validation/control-replay/`.

**Tests & exit — M3 complete.** All runtime parameters (source positions,
rotation, decoder trims, mutes) drivable and readable over OSC per the map
doc; head-tracker quaternion rotates the field within the ≤ 2-buffer
response target; `osc_replay.py` green in CI; PRD §9's head-tracker-latency
caveat documented in the map doc (playback-grade, not VR-grade).

**Risks.** Address-scheme churn (that's why the map freezes first);
rate-limiter interaction with encode-matrix ramps (test fast position
streams).

---

### WP10 — GUI framework port, XOA tabs, rV/rE visualization (XL)

**Goal.** Replace the throwaway shell with the real application UI, built
from WFS-DIY's portable kit, including the decoder-inspection plots that
make FR-18 whole.

**PRD coverage.** FR-18 (plots), FR-10 (dials), FR-16 (suggestion UI),
§7 latency display; the visualization half of M5.

**Tasks.**
- Port the portable kit from `d:/dev/WFS_DIY_v1/Source/gui/`:
  `WfsLookAndFeel.h` → `XoaLookAndFeel`, `ColorScheme.h`, `StatusBar.h`,
  `LevelMeterWindow.h`, `PatchMatrixComponent.*`, `HelpCard.h` /
  `HelpCardSVG.h`, the `dials/` / `sliders/` / `buttons/` widget kits,
  `Source/Localization/LocalizationManager.*`,
  `Source/Accessibility/TTSManager.h`.
- **Explicitly do not port** the WFS tab headers (`InputsTab.h` 438 KB
  etc.) — they are WFS-shaped; XOA tabs are built fresh on the kit.
- XOA tabs (the fixed scope list): **System Config** / **Network** /
  **Inputs** (encoder: position, gain, spread, NFC) / **Speakers +
  Decoder** (layout editor, decoder type + suggestion, dual-band, trims,
  test signals) / **Monitoring** / **Map** (source view).
- rV/rE sphere plots + energy maps fed by WP5's `AmbiRvReAnalysis` data;
  decode-matrix export button (FR-18).
- Speaker-layout editor (adapt `OutputArrayHelperWindow` ring/dome/grid
  preset generators to 3-D rigs).
- Latency readout, decoder-rebuild progress (non-blocking indicator).
- Retire the WP6 shell.

**Tests & exit.** Every v1 parameter reachable from the GUI (cross-check
against the WP2 schema tables); rV/rE plots numerically match WP5's CSV
export for the fixtures; help-card + localization scaffolds working; Map
view time-boxed — a 2-D projection is acceptable for v1.

**Risks.** Scope creep — the tab list above *is* the scope; anything else
is post-v1.

---

### WP11 — GPU decode path — **M4** (XL, cross-repo)

**Goal.** GPU as the default decode path with mandatory CPU fallback
(PRD §5: decode is the ideal GPU workload — dense GEMM, fat buffers,
latency-tolerant), profiled against §7 targets.

**PRD coverage.** FR-17 (hot swap on the GPU path), PRD §5 GEMM notes,
§7 GPU targets; M4 exit. **Scheduled after WP10 by decision D6** — the one
deviation from PRD milestone order; the spatcore-side track runs in
parallel with WP9/WP10 once WP7 freezes the matrix contracts.

**Track D1 — spatcore repo** (never from this repo; lands here via pin bump):
- New kernel family `IAmbiBackend` following the documented additive
  extension recipe (`I<Family>Backend : IGpuBackend`): **decode GEMM
  first** ([out × 121]·[121 × block], dual-band as two matrices or the
  factorized single-GEMM per WP7); fused encode+rotate+decode is a stretch,
  not the baseline.
- Cuda/Hip/Metal implementations, kernels runtime-compiled (NVRTC/hipRTC/
  MSL) as byte-frozen string headers; `GpuBackendFactory` registration;
  plugin build scripts (`tools/gpu/build-gpu-plugins.*`); spatcore version
  bump. Budget **≥ 2 pin-bump cycles** (API lands; fixes follow).

**Track D2 — XOA repo:**
- Pin bump to the `IAmbiBackend`-bearing spatcore.
- `Source/DSP/NativeGpuAmbiAlgorithm.h` — same duck-typed contract as
  `AmbiBusAlgorithm`, wrapping `AmbiGpuBackend` + `GpuAsyncPipelineT`,
  following the `spatcore/wfs/NativeGpuWfsAlgorithm.h` recipe (enum switch
  in the app selects CPU/GPU; **CPU fallback mandatory**, FR/PRD §5).
- Offline-render GPU paths: synchronous backend drive, separate per-machine
  `baselines/<machine>-gpu.json`, self-consistency gate (exactly the
  WFS-DIY pattern), plus CPU-vs-GPU null within a documented FP tolerance.
- Port `tools/validation/kernel_hashes.py` gate.
- Profiling report vs §7: Nsight SM/memory-throughput numbers; targets
  < 15 % of an RTX-5070-class GPU at the reference shape, T4-class viable,
  CPU fallback < 40 % on a 9950X-class.

**Tests & exit — M4.** GPU decode bit-gated against its own baseline;
CPU/GPU null within tolerance; decoder hot-swap intact under GPU; §7
performance table filled in with measured numbers. **CI runners have no
GPUs** — GPU gates run locally / on a self-hosted box and say so in the
harness README.

**Risks.** Cross-repo cadence (mitigated by the parallel track + budgeted
pin bumps); fusion temptation (baseline is decode-GEMM only); GPU CI gap
(documented, per-machine gates).

---

### WP12 — MCP server + AI undo — M5 part (M)

**Goal.** The AI control surface: XOA's parameter CSV → generated MCP tools
on spatcore's dispatcher, with tiers and undo.

**PRD coverage.** FR-23 (~60–100 tools expected).

**Tasks.**
- `Source/Network/MCP/` — port WFS-DIY's `MCPServer` wiring onto spatcore's
  `MCPDispatcher` / `MCPToolRegistry` / `MCPTierEnforcement` /
  `MCPUndoHooks` / `MCPResourceRegistry` / `MCPTransport`
  (Streamable HTTP); port `MCPUndoEngine`, `MCPGeneratedToolLoader`,
  `MCPParameterRegistry`.
- Author XOA parameter CSV inventories `Documentation/XOA-UI_*.csv`
  (tab-separated, one per schema section — the WFS-DIY convention).
- `tools/generate_mcp_tools.py` thin wrapper + `tools/mcp/xoa_codegen_config.py`
  (namespaces, abbreviation maps, tier tables) feeding the app-agnostic core
  `spatcore/tools/codegen/generate_mcp_tools.py` → committed
  `generated_tools.json` / `generated_groups.json`.
- Port `MCPUndoOverlay` / `MCPHistoryWindow` GUI (needs the WP10 kit).
- Tier configuration (three-tier confirmation) per parameter class.
- CI check: regenerate and diff `generated_tools.json` so CSV/schema drift
  fails the build.
- Port `tools/validation/control-replay/mcp_replay.py` with XOA fixtures.

**Port sources.** `d:/dev/WFS_DIY_v1/Source/Network/MCP/*`,
`Source/gui/MCPUndoOverlay.*`, `MCPHistoryWindow.*`,
`tools/mcp/wfs_codegen_config.py`, `Documentation/WFS-UI_*.csv` (as format
reference).

**Tests & exit.** Tools generate deterministically from CSV; an end-to-end
MCP tool call moves a parameter with origin-tagged undo and tier-2/3
confirmation handshake; `mcp_replay.py` green; generate-and-diff CI check
active.

**Risks.** CSV authoring discipline vs the WP2 schema (the CI diff check is
the mitigation).

---

### WP13 — Acceptance, hardening, performance — **v1.0** (L)

**Goal.** Make PRD §10 literally true, with committed evidence.

**PRD coverage.** §10 verbatim; §7 table; FR-20 architecture check.

**Tasks.**
- Scripted acceptance runs: {3rd-order AmbiX file, synthetic 10th-order
  scene} × {24-ring, hemispherical dome, irregular shoebox} × {CPU, GPU} ×
  {with rotation} — null-test vs reference decoder matrices, rV/rE within
  the WP5 tolerances.
- 1-hour xrun-free soak at the §7 shapes (order 10, 121 ch, 96 kHz,
  250 outs, 1024 samples) with xrun/latency logging; committed logs.
- 256-output architecture check (FR-20): configuration loads, matrices
  size correctly, no hardcoded ceilings below 512.
- End-to-end latency measured and reported in UI vs ≤ 30 ms (§7).
- Optional S-sized stretch (D2): utility monitor decode — static
  virtual-speaker stereo downmix, no HRTF/SOFA.
- Bug-fix buffer; first-pass user documentation.

**Tests & exit — v1.0.** The PRD §10 acceptance sentence holds, evidenced
by committed acceptance logs and baselines; §7 table filled with measured
numbers (GPU and CPU).

**Risks.** PRISM rig scheduling for the dome/listening checks (book early);
order-10 content (already solved by the WP6 generator).

---

### WP14 — Ship (M)

**Goal.** Tagged releases produce installable artifacts on all three OSes.

**Tasks.**
- `Installer/XOA-Installer.iss` — adapt WFS-DIY's Inno Setup 6 script
  (version via `/DMyAppVersion`, GPL page, x64).
- macOS notarized pkg (adapt `Scripts/ci/build-macos-release.sh` pattern);
  Linux tarball (port `tools/linux/build-app-tarball.sh`).
- `.github/workflows/release.yml` — port and adapt from WFS-DIY.
- GPU plugin packaging: per-vendor `libwfs_<vendor>` plugins via the
  spatcore `tools/gpu/build-gpu-plugins.*` scripts, installed to the
  layout the `GpuBackendFactory` dlopen expects.
- README / user manual, `THIRD_PARTY_NOTICES.md` final pass, version
  stamping from the CMake project version.

**Exit.** A tag produces installers/tarballs on the three OSes; a clean
machine installs and runs; GPU plugins load from the installed layout.

---

## 6. Test & CI evolution timeline

| Stage | Introduced in | CI change |
|---|---|---|
| Unit tests (CHECK-macro console app, ctest) | WP1 | test step on all 3 OS jobs (Debug; `ctest -C Debug` on multi-config) |
| Offline-render harness | WP6 | harness builds on CI + render smoke; `--check` against sha256 baselines stays **per-machine** locally (WFS-DIY pattern); promote to per-OS CI baselines only once cross-machine FP stability is proven (`spatcore_apply_compile_flags` is the enabler) |
| Control replay (`osc_replay.py`) | WP9 | replay job against a headless app run |
| MCP replay + generate-and-diff | WP12 | codegen drift check + `mcp_replay.py` |
| GPU gates (`-gpu.json` baselines, kernel hashes) | WP11 | **local/self-hosted only** — hosted CI runners have no GPUs; documented in the harness README |
| Release-config builds, artifact upload, soak evidence | WP13 | add Release matrix + artifacts |
| Release workflow | WP14 | `release.yml` on tags |

Framework decision, restated once: the dependency-free CHECK pattern from
`spatcore/tests/SpatcoreTests.cpp`, not gtest/Catch2 — consistent with
spatcore and with the repo's no-new-dependencies posture.

---

## 7. Acceptance matrix and FR coverage

### PRD §10 acceptance, decomposed

| §10 element | Introduced | Verified |
|---|---|---|
| 3rd-order AmbiX file decodes correctly | WP6 | WP13 |
| Synthetic 10th-order scene | WP6 (generator) | WP13 |
| Null-test vs reference decoder matrices | WP5 (CPU goldens), WP11 (GPU) | WP6, WP13 |
| rV/rE within tolerance | WP5 (tolerances defined) | WP13 |
| (a) 24-speaker ring | WP5 (fixture) | WP13 |
| (b) hemispherical dome | WP7 (imaginary floor) | WP13 |
| (c) irregular shoebox rig | WP7 | WP13 |
| With rotation | WP4/WP6 | WP13 |
| GPU and CPU paths | WP11 | WP13 |
| Xrun-free 1 h at §7 targets | — | WP13 |

### FR → WP coverage index

| FR | Where |
|---|---|
| FR-1 native ACN/SN3D, 121 ch | WP3 (constants from WP0) |
| FR-2 boundary conversion (N3D/FuMa) | WP3 (math) + WP6 (runtime) |
| FR-3 fixed order-10 bus, order-generic code | §3 rule; enforced from WP3 on |
| FR-4 44.1–96 kHz, SR-agnostic filters | WP6 (engine), WP7 (LR4), WP8 (NFC) |
| FR-5 mono encoding + spread, click-free | WP8 |
| FR-6 distance / NFC | WP8 |
| FR-7 HOA stream up/downmix | WP3 (weights) + WP6 (chain) |
| FR-8 file playback ≤ 128 ch, AmbiX metadata | WP6 |
| FR-9 SO(3) rotation (Ivanic–Ruedenberg) | WP4 (math) + WP6 (RT apply) |
| FR-10 rotation sources: dials / OSC / tracker | WP6 (dials), WP9 (OSC + quaternion), WP10 (full UI) |
| FR-11 mirror; zoom v1.1 | WP4 (mirror); zoom **parked** §8 |
| FR-12 SAD + mode-matching | WP5 |
| FR-13 AllRAD + imaginary speakers | WP7 |
| FR-14 dual-band basic/max-rE, LR crossover | WP7 |
| FR-15 per-speaker delay/gain/trim/mute/solo/EQ | WP7 |
| FR-16 layout definition, WFS-DIY import, suggestion | WP2 (schema) + WP5 (import/detect) + WP10 (editor UI) |
| FR-17 precomputed, validated, hot-swapped matrices | WP5 (+ WP11 GPU swap) |
| FR-18 rV/rE plots, energy maps, matrix export | WP5 (data/export) + WP10 (plots) |
| FR-19 device I/O (ASIO/CoreAudio/ALSA/JACK) | WP6 (AudioEngine on JUCE device layer) |
| FR-20 256 outs, clean to 512 | WP6 (architecture) → verified WP13 |
| FR-21 test signals, speaker-ID | WP7 (new `TestSignalGenerator`) |
| FR-22 OSC in/out all runtime params | WP9 |
| FR-23 MCP server | WP12 |
| FR-24 project files (XML per D1), WFS-compatible layout | WP2 |

Binaural monitoring (PRD non-goal / XOA-PLAN P6) is **parked** — see §8.

---

## 8. Post-v1 backlog

In rough priority order:

1. **Binaural monitoring** (was P6): magLS or HRTF convolution of a virtual
   layout, SOFA loading, head-tracked via the WP9 `RtSnapshot` seam.
2. **Zoom/focus warping** (FR-11 v1.1): order-weighted soundfield warping.
3. **Tracker profiles + OSCQuery** (P3 tail): PSN/RTTrP/MQTT receivers
   (vendor PSN-CPP when needed), OSCQuery server.
4. **User-placeable imaginary speakers** for AllRAD on severe shoeboxes
   (promote into v1.x if the WP7 listening checkpoint demands it).
5. **SH-domain reverb**: spatcore FDN/IR families on the 121-ch bus
   (spatcore docs flag the SDN 32-node cap — prefer FDN/IR).
6. **JSON project export** (D1 mitigation) alongside the XML files.
7. **A→B conversion** (microphone processing).
8. **`spatcore/ambi/` extraction**: move the stabilized `Source/DSP/Ambi*`
   modules into spatcore behind bit-exact baselines (spatcore-repo project +
   version bump) — completing the prototype-then-extract lifecycle.
9. DAW plugin versions; Linux multitouch JUCE patch set (only if XOA
   targets touch UIs on Linux).

---

## 9. Appendix — references and golden-data generation

Literature the implementer will need at the keyboard:

- **Ivanic, J. & Ruedenberg, K.** (1996), *Rotation Matrices for Real
  Spherical Harmonics*, J. Phys. Chem. 100(15) — **with the 1998 erratum**
  (J. Phys. Chem. A 102(45)). Implement from the corrected tables only.
- **Daniel, J.** (2003), *Spatial sound encoding including near field
  effect* — NFC-HOA filters; the reference curves WP8 validates against.
- **Zotter, F. & Frank, M.** (2012), *All-Round Ambisonic Panning and
  Decoding*, JAES 60(10) — AllRAD, imaginary speakers; and their max-rE
  weight treatment (*Ambisonics*, Springer 2019, ch. 4).
- **Kronlachner, M.** (2014), master's thesis, IEM Graz — order-weight
  widening/spread and warping.
- **Nachbar, C., Zotter, F., Deleflie, E., Sontacchi, A.** (2011),
  *AmbiX — A Suggested Ambisonics Format* — ACN/SN3D conventions.
- **Pulkki, V.** (1997), *Virtual Sound Source Positioning Using VBAP*,
  JAES 45(6).

Golden-data generators (all in `tools/reference/`, scipy/numpy versions
pinned in each script header, outputs committed to `tests/data/`):

| Script | Produces | Consumed by |
|---|---|---|
| `gen_sh_reference.py` | real-SH values (documents the scipy complex→real, Condon–Shortley conversion **in code**) | WP3 |
| `gen_maxre_weights.py` | exact max-rE weights, orders 1–10 | WP3 |
| `gen_rotation_reference.py` | rotation/Wigner-D matrices for random orientations | WP4 |
| `gen_decoder_reference.py` (octave/IEM-assisted, provenance documented) | SAD/mode-matching golden matrices for the ring + dome fixtures | WP5 |
| `gen_tdesign_tables.py` | virtual-layout t-design coordinates + provenance | WP7 |
| `gen_bessel_roots.py` | reverse-Bessel polynomial roots ℓ = 1…10 | WP8 |
| `gen_nfc_reference.py` | NFC magnitude-response curves (Daniel) across SR/radius | WP8 |
