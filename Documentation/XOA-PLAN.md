# XOA — Architecture & Roadmap

> XOA = tenth-order Ambisonics (10OA). The Ambisonics sibling of
> [WFS-DIY](https://github.com/pob31/WFS-DIY), built on the shared
> [spatcore](https://github.com/pob31/spatcore) engine. spatcore's own
> boundary docs (`spatcore/docs/core-boundary-proposal-audio.md`) were written
> with XOA as a named consumer: "XOA (10th-order Ambisonics → 121 SH
> channels)".

## 1. Fixed conventions

| Thing | Choice |
|---|---|
| Order | 10 → **121 SH channels** ((N+1)²) |
| Channel ordering | **ACN** |
| Normalization | **SN3D** (AmbiX convention) |
| Signal flow | N sources → **encode** (121-ch SH bus) → **field transforms** (rotate / mirror / zoom) → **decode** → M speakers, and/or **binaural decode** for monitoring |
| Build | CMake-native consumer of spatcore (`spatcore-audio`, `spatcore-control`, `spatcore-controllers`) |
| License | GPLv3 |

## 2. The spatcore seams XOA plugs into

Established by reading the WFS-DIY v1 integration (reference checkout:
`d:/dev/WFS_DIY_v1`):

1. **Renderer "algorithm" contract** — duck-typed, no base class. Every
   renderer implements the same method surface so the app can hold them as
   value members and switch on an enum:
   `prepare(numIn, numOut, sampleRate, blockSize, <const float* param matrices...>, enabled)` ·
   `processBlock(const AudioSourceChannelInfo&, const AudioBuffer<float>& in, numIn, numOut)` ·
   `setProcessingEnabled` · `releaseResources` · metering accessors.
   Templates to copy: `spatcore/wfs/InputBufferAlgorithm.h` (+`InputBufferProcessor.h`)
   for the CPU worker-thread pattern; `spatcore/wfs/NativeGpuWfsAlgorithm.h`
   for the GPU async-pipeline wrapper.
2. **Parameter delivery, control → RT**: two mechanisms.
   (a) *Live matrices*: app-owned flat `std::vector<float>` matrices, raw
   `const float*` handed to the renderer at `prepare()`; a message-thread
   timer rewrites them in place; workers read them live (single-writer
   invariant, benign staleness). WFS uses `[in × out]` delay/level/HF — XOA
   uses `[in × 121]` **encode coefficients** and `[121 × out]` **decode
   matrix** through the same seam.
   (b) *`spatcore::rt::RtSnapshot<T>`*: POD snapshot publish/acquire for
   everything that isn't a plain matrix — the model for **rotation state**
   (orientation quaternion / precooked rotation coefficients). Exemplar:
   WFS-DIY `Source/DSP/BinauralCalculationEngine.h` (`RtParams`).
3. **Per-sample smoothing**: `spatcore/dsp/DelayTargetSmoother.h` (C1 smoother
   + teleport envelope) for click-free coefficient changes; biquads
   (`OutputEQBiquadFilter`) for decoder shelf/EQ; `TrackingPositionFilter`
   (1-Euro) for incoming source/head positions.
4. **Parameter store**: derive
   `spatcore::control::state::TreeParameterStore` (as WFS-DIY's
   `WFSValueTreeState` does) with an XOA schema; persistence via
   `XmlPersistence`.
5. **Control plane**: `spatcore/control/osc` (transports, codec, ingest,
   rate limit) and `spatcore/control/mcp` (JSON-RPC dispatcher, tool
   registry, tiers, undo hooks) are app-agnostic; the app supplies the
   schema-specific routing/tool layer (WFS-DIY generates its MCP tools from
   CSV inventories — reuse that generator).
6. **GPU (later)**: kernel families are `I<Family>Backend : IGpuBackend` with
   Cuda/Hip/Metal implementations compiled at runtime (NVRTC/hipRTC/MSL) and
   pumped by the family-agnostic `GpuAsyncPipelineT`. Adding an `IAmbiBackend`
   (encode/rotate/decode as fused matrix kernels) is an additive, documented
   extension — no pipeline changes.

**What spatcore does *not* have yet:** spherical-harmonic evaluation,
SH rotation, decoder design (sampling/mode-matching/AllRAD, max-rE weights),
near-field compensation filters. That is XOA's new DSP.

## 3. Where the Ambisonics DSP lives

Follow the same lifecycle that produced spatcore: **prototype app-local**
(`Source/DSP/Ambi*`), stabilize the encode/rotate/decode contracts against
offline-render tests, **then extract to `spatcore/ambi/`** (a spatcore
version bump) once WFS-DIY-style bit-exactness baselines exist. Iterating
inside the shared pinned submodule from day one would churn spatcore pins for
every experiment.

## 4. Roadmap

- **P0 — Bootstrap (this repo state).** Submodules (JUCE 8.0.14 @2cdfca8f,
  spatcore @9053821 / 0.1.1-pre, hidapi @0.15.0), vendored juce_simpleweb +
  roli_blocks_basics, CMake build of a minimal JUCE app linking
  spatcore-audio/-control/-controllers, GPLv3, CI build sanity green on the
  three OSes.
- **P1 — Parameter store & schema.** `XoaValueTreeState :
  TreeParameterStore`; sections: Config (show/IO/stage/master/network),
  Inputs (position in cart/cyl/spherical via `CoordinateConverter`, gain,
  width/spread, NFC on/off), Speakers (layout geometry, per-output trim/delay/EQ),
  Decoder (type, dual-band, max-rE), Monitoring (binaural). Project file I/O
  via `XmlPersistence`. Port `WFSFileManager` pattern.
- **P2 — CPU Ambisonics engine v1.**
  - SH library: ACN/SN3D real SH evaluation (stable recurrences up to n=10),
    encode-coefficient computation per source; unit tests against known
    values + orthonormality sums.
  - `AmbiCalculationEngine` (control side): positions → `[in × 121]` encode
    matrix; decoder design → `[121 × out]` decode matrix (start: sampling
    decoder + max-rE weighting on regular-ish layouts; AllRAD later).
  - `AmbiAlgorithm` (RT side): implements the spatcore algorithm contract;
    encode-mix to a 121-ch bus, rotate (`RtSnapshot` orientation), decode.
    Smoothing via `DelayTargetSmoother`/linear ramps per coefficient.
  - Offline render baselines from day one (the WFS-DIY validation discipline).
- **P3 — Control plane.** OSC manager + address map (mirror the ADM-OSC
  subset where it fits), tracking receivers (RTTrP/PSN — vendor PSN-CPP when
  needed), OSCQuery.
- **P4 — MCP server.** Port `MCPServer`/undo overlay wiring; author XOA's
  parameter CSV and reuse the WFS-DIY `tools/generate_mcp_tools.py`
  generator (spatcore `tools/codegen` holds the core).
- **P5 — GUI.** Port the WFS-DIY GUI framework (LookAndFeel, ColorScheme,
  StatusBar, meters, patch matrix, help cards, localization, TTS) and build
  XOA tabs: System Config / Network / Inputs (encoder) / Speakers+Decoder /
  Monitoring / Map (3-D-ish source view).
- **P6 — Binaural monitoring.** HOA→binaural (magLS or HRTF-convolution of a
  virtual layout), `RtSnapshot`-driven head orientation.
- **P7 — GPU family.** `IAmbiBackend` (CUDA/HIP/Metal): fused
  encode+rotate+decode as batched GEMV-ish kernels; 121×M matrices are ideal
  GPU shapes. Follow `NativeGpuWfsAlgorithm` + `GpuBackendFactory` extension
  recipe; gate bit-exact against the CPU renders.
- **P8 — Ship.** Installers (mirror WFS-DIY Inno Setup / notarized pkg /
  tarball), release CI, docs.

## 5. Open questions (decide when reached)

- Decoder set for v1: sampling+max-rE only, or include mode-matching? AllRAD
  needs a spherical triangulation (vendor a small lib vs implement).
- NFC (near-field compensation): order-10 NFC filters are numerically spicy —
  which realization (IIR cascades per degree), and at what stage.
- Reverb: reuse spatcore FDN/IR in the SH domain (121-ch feeds are heavy;
  spatcore docs flag the SDN 32-node cap — prefer FDN/IR families).
- Whether the WFS-DIY Linux JUCE multitouch patch set gets ported (only if
  XOA targets touch UIs on Linux).
