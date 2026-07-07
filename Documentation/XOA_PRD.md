# XOA — Product Requirements Document
**Higher-Order Ambisonics Processor (up to 10th order)**
Version 0.1 — July 2026 — GPL-3.0 — C++ / JUCE / spatcore

---

## 1. Purpose and Positioning

XOA is a standalone Higher-Order Ambisonics (HOA) processor for encoding, transforming, and decoding Ambisonics program material up to **10th order (121 channels)** onto real loudspeaker rigs — regular (sphere/dome/ring) or irregular (shoebox rooms, partial coverage, asymmetric installs).

XOA is a sibling of WFS-DIY, not a mode inside it. It shares the **spatcore** submodule (audio engine, RT-safety patterns, GPU backends, control plane) but is a separate product with its own signal flow and UI.

Design context that shapes every decision below:

- **Recorded / streamed material, not live reinforcement.** Latency budget is relaxed (tens of ms acceptable). This permits fat buffers (512–2048 samples) and puts the **GPU in the main signal path** by default, unlike WFS-DIY's hybrid split.
- **Primary validation target:** PRISM lab's HOA dome (10th-order-capable rig) and generic venue rigs from 8 speakers upward.
- **Interchange standards first:** AmbiX (ACN / SN3D) is the native convention. Anything else is converted at the boundary.

### Non-goals (v1)

- Live low-latency reinforcement (that is WFS-DIY's job).
- Binaural monitoring beyond a basic utility decode (nice-to-have, section 12).
- Object-based scene authoring / DAW plugin versions.
- B-format microphone processing beyond stream ingest (no A→B conversion in v1).

---

## 2. Users and Primary Scenarios

1. **Composer / sound designer** plays back an AmbiX file (order 1–10) on the venue rig, rotates and re-orients the scene live, adjusts decoder character (basic / max-rE), and trims levels.
2. **Research engineer (PRISM)** feeds a 121-channel 10th-order stream over Dante/MADI to the dome, validates decoder output channel-by-channel, measures with the analysis rig, and swaps decoder types for listening comparison.
3. **Theatre integrator** encodes N mono stems into HOA, positions/moves them (azimuth, elevation, distance), and decodes to an irregular shoebox rig that no regular decoder fits.

---

## 3. Signal Flow Overview

```
                       ┌──────────────────────────────────────────────┐
 Mono sources ──► Encoder(s) ──►│                                      │
                       │        Σ  HOA bus (order 10, 121 ch, ACN/SN3D)│──► Decoder ──► Speaker outs
 HOA stream  ──► Order adapt ──►│   Rotation / Mirror / Zoom (SO(3))   │      │
 (file/Dante)    (up/down)      └──────────────────────────────────────┘      ├─► Per-speaker delay/gain/EQ
                                                                              └─► Meters, solo, test signals
```

One HOA bus per project. All inputs are encoded to / adapted to the bus, transformed on the bus, then decoded once. The bus is fixed at **order 10 (121 channels)** — the project's native order (`XoaConstants.h`); lower-order content is adapted up to it, so there is no user-selectable bus order.

---

## 4. Functional Requirements

### 4.1 Conventions and formats

- **FR-1** Native convention: **ACN channel ordering, SN3D normalization** (AmbiX). Channel count = (N+1)²; N=10 → 121 channels.
- **FR-2** Ingest conversion at the boundary: N3D↔SN3D scaling, FuMa→ACN/SN3D for orders ≤ 3 (legacy import only; flagged in UI). Internally everything is ACN/SN3D.
- **FR-3** Bus order is fixed at **10 (121 channels)** — the native order. Supported *content* orders are any integer 1–10 (adapted to the bus). Decoder and all transforms must be order-generic: no hardcoded per-order tables; sizes derive from the order constant in `XoaConstants.h`.
- **FR-4** Sample rates 44.1–96 kHz; sample-rate agnostic DSP (all filters designed at runtime for the active rate).

### 4.2 Inputs

- **FR-5 Mono source encoding.** Any input channel can be encoded to the bus with parameters: azimuth, elevation, distance, gain, and optional spread (order-reduction blur). Real spherical harmonics evaluated per source; smooth parameter interpolation (click-free, ≤ one buffer ramp).
- **FR-6 Distance / near-field.** Distance handling via gain law + optional **NFC (near-field compensated) encoding filters** (Daniel) matched to the rig's mean radius. NFC filters must be stable at order 10 / 44.1 kHz — implement as cascaded first/second-order sections with per-order cutoffs, not naive transfer functions.
- **FR-7 HOA stream input.** Accept an AmbiX stream (file or network audio) of order M against the fixed order-10 bus:
  - **Upmix (M < 10):** zero-pad higher-order coefficients (standard practice; creates no false spatial detail, preserves compatibility). Optional gentle order-weighting shelf to avoid brightness mismatch.
  - **Downmix (M > 10):** truncate to order 10 with max-rE re-weighting to minimize truncation artifacts.
- **FR-8 File playback.** Multichannel WAV/CAF/FLAC up to 128 channels, with AmbiX metadata detection where present; manual order/convention override always available.

### 4.3 Scene transforms (on the HOA bus)

- **FR-9 Rotation.** Full SO(3) rotation (yaw/pitch/roll or quaternion) of the entire soundfield, implemented with **recursive per-order rotation matrices (Ivanic–Ruedenberg)**, block-diagonal per order. Rotation is a hot path: matrices recomputed on parameter change, applied every block; coefficient interpolation between old/new matrices over one buffer to stay click-free.
- **FR-10 Rotation sources:** UI dials, OSC, and head-tracker / sensor input (generic OSC quaternion; specific tracker profiles later).
- **FR-11 Mirror** (flip left/right, front/back, up/down — trivial sign flips per m) and **global order-weighted zoom/focus** (warping) as a stretch goal (v1.1), not v1.0.

### 4.4 Decoding

- **FR-12 Regular layouts** (rings, domes, spheres, platonic-ish): **Sampling (SAD)** and **Mode-Matching (pseudo-inverse)** decoders, selectable.
- **FR-13 Irregular layouts (the shoebox case): AllRAD** (All-Round Ambisonic Decoding, Zotter–Frank) is the required default — decode to a virtual t-design, then VBAP the virtual sources to the real rig, with **imaginary loudspeakers** inserted to stabilize gaps (e.g., missing floor coverage). This is the decoder that makes "any rig" honest.
- **FR-14 Dual-band decoding:** basic (velocity) weighting below crossover, **max-rE** (energy) weighting above; crossover frequency user-adjustable (default ~400 Hz for typical rigs, scaled by rig radius); phase-matched Linkwitz–Riley band split.
- **FR-15 Per-speaker compensation:** distance delay alignment, distance gain (1/r or user law), per-speaker trim and mute/solo, optional per-speaker EQ (biquad chain, reuse spatcore filter blocks).
- **FR-16 Layout definition:** reuse WFS-DIY's speaker-setup model via spatcore (positions in meters, groups, per-speaker metadata). Import from WFS-DIY project files. Regularity is *detected and suggested*, never assumed: the app recommends SAD/mode-matching vs AllRAD but the user can override.
- **FR-17 Decoder matrices are precomputed** (non-RT) on layout/order/decoder-type change, validated (condition number check for mode-matching; warn on ill-conditioned layouts), then hot-swapped atomically (RtSnapshot pattern).
- **FR-18 Validation outputs:** per-speaker rV/rE vector plots and energy maps over the sphere so a decoder can be *seen* before it is heard. Export decode matrix as CSV/JSON.

### 4.5 Outputs and I/O

- **FR-19** Output to system audio devices, ASIO (Windows), Core Audio (macOS), ALSA/JACK (Linux) — via JUCE + spatcore device layer. Primary deployment: **RME HDSPe AoX (Dante/MADI)** and Digiface Dante.
- **FR-20** Channel counts: up to 256 physical outputs v1 (PRISM dome scale), architecture clean up to 512.
- **FR-21** Test signals per speaker (pink noise burst, sweep) and speaker-identification mode, reused from WFS-DIY where present in spatcore.

### 4.6 Control plane

- **FR-22 OSC** in/out for all runtime parameters (source positions, rotation, decoder trims), address scheme consistent with WFS-DIY conventions.
- **FR-23 MCP server** (reuse spatcore control-plane framework: Streamable HTTP, auto-generated tool surface from parameter CSV, three-tier confirmation, AI undo/redo). XOA ships with its own parameter CSV; tool count will be far smaller than WFS-DIY's (~60–100 tools expected).
- **FR-24** Show/project files (JSON, human-diffable), compatible layout section with WFS-DIY.

---

## 5. DSP Specification Notes (implementation guidance)

- **Encoding** = per-source vector of 121 SH evaluations × gain; trivially parallel; CPU is fine for dozens of sources, GPU batch-encode available for large source counts.
- **Rotation** = block-diagonal matrix multiply; per order ℓ the block is (2ℓ+1)×(2ℓ+1). Total ~1.8k MACs/sample at order 10 (Σₗ (2ℓ+1)² = 1771) — cheap; keep on CPU for latency of parameter response, GPU optional.
- **Decoding** = single **GEMM**: [L speakers × 121] · [121 × block]. At 250 outs, 96 kHz, 1024-block: ~31 MMAC/block ≈ 3 GFLOP/s sustained — small for any discrete GPU, comfortable even on the T4-class card, and the *ideal* GPU-shaped workload (dense GEMM, fat buffer, latency-tolerant). **GPU is the default decode path; CPU fallback mandatory** (same code shape via spatcore backend abstraction: CUDA / Metal / HIP).
- **Dual-band** doubles the GEMM (two matrices, band-split inputs) — still trivial.
- **NFC filters:** per-order IIR cascades applied on the bus (121 channels, ≤ 10 biquad-equivalent sections each) — CPU-friendly, GPU optional.
- All RT/non-RT boundaries follow spatcore rules: no ValueTree reads on RT threads; POD snapshots; lock-free parameter queues (TrackingIngestQueue pattern for external control streams).

---

## 6. spatcore Reuse Map

| spatcore asset | XOA use |
|---|---|
| Audio device/engine layer, buffer management | as-is |
| RtSnapshot POD publishing, RT-safety rules | all decoder/rotation matrix swaps |
| GPU backend abstraction (CUDA/Metal/HIP) + batched H2D/D2H transfer worker | decode GEMM, batch encode |
| Runtime CPU topology enumeration / thread placement | RT thread pinning |
| Speaker layout model + editor widgets | layout definition, imported from WFS-DIY |
| OSC framework | FR-22 |
| MCP control-plane generator | FR-23 |
| Per-speaker delay/gain/EQ blocks | FR-15 |
| Project file I/O | FR-24 |

New, XOA-specific modules: SH evaluation, rotation matrices, decoder designers (SAD / mode-matching / AllRAD+imaginary), NFC filters, rV/rE visualization.

---

## 7. Performance Targets

| Metric | Target |
|---|---|
| Bus order 10, 121 ch, 96 kHz, 250 outs, GPU decode | < 15 % of RTX-5070-class GPU; runs on T4-class |
| Same on CPU fallback (9950X-class) | < 40 % CPU, no xruns at 1024 samples |
| End-to-end latency (file → output) | ≤ 30 ms acceptable; report actual in UI |
| Rotation parameter → audio response | ≤ 2 buffers |
| Decoder rebuild (order 10, 250 spk, AllRAD) | ≤ 2 s, non-blocking |

---

## 8. Milestones

1. **M0 — spatcore split complete** (shared engine builds standalone; WFS-DIY consumes it unchanged).
2. **M1 — Core bus:** AmbiX file playback, order adapt, rotation, SAD decode to regular ring, CPU only. *First audible milestone.*
3. **M2 — AllRAD + dual-band + per-speaker comp** (the shoebox works).
4. **M3 — Mono encoders + NFC + OSC.**
5. **M4 — GPU decode path + profiling** (Nsight SM/memory throughput report vs targets).
6. **M5 — MCP server, rV/rE visualization, project I/O, beta.**

---

## 9. Risks / Open Questions

- **AllRAD quality on severe shoeboxes** (no height at all): imaginary-speaker strategy needs listening validation; may need user-placeable imaginary speakers.
- **NFC at order 10** is numerically delicate near Nyquist and at small radii — validate against Daniel's reference curves early (M3).
- **FuMa beyond order 3** is non-standard — explicitly out of scope; reject with a clear message.
- **Head-tracker latency** over OSC for rotation: acceptable for playback use; document, don't promise VR-grade.
- Order-10 *content* is rare; provide an internal test-scene generator (encoded moving sources) for validation without external material.

---

## 10. Acceptance (v1.0)

A 3rd-order AmbiX file and a synthetic 10th-order scene both decode correctly (null-test vs reference decoder matrices, rV/rE within tolerance) on (a) a 24-speaker ring, (b) a hemispherical dome, (c) an irregular shoebox rig — with rotation, on GPU and CPU paths, xrun-free for 1 hour at the performance targets above.
