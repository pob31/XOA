# XOA — OSC Address Map

Version 0.1 — July 2026 — GPL-3.0

This document is the **frozen contract** for XOA's OSC control surface (WP9,
FR-22 / FR-10 / D18-FR-25). It is written before the network code so the
address scheme cannot drift chunk-to-chunk (DEVPLAN §WP9: "freeze the address
map first"). Every address below maps to a real parameter id in
`Source/Parameters/XoaParameterIDs.h`; ranges and enum meanings come from
`Source/Parameters/XoaParameterDefaults.h`.

Scope for v1 (decision D3): a **generic OSC quaternion** head-tracker and a
**listener position** (D18). Explicitly **out of scope** for v1: PSN / RTTrP /
MQTT tracker profiles, OSCQuery, and zeroconf discovery — see §11.

---

## 1. Transports and ports

| Direction | Transport | Bind / target | Port param | Default |
|---|---|---|---|---|
| Inbound | UDP | local bind | `oscReceivePort` | **9000** |
| Inbound | TCP (length-prefixed) | local bind | `oscTcpPort` | **9002**, gated by `oscTcpEnabled` (default off) |
| Outbound | UDP | `oscSendAddress` : `oscSendPort` | — | **127.0.0.1 : 9001** |

- TCP framing is a 4-byte big-endian length prefix before each OSC packet
  (spatcore `OSCTCPReceiver`, ≤ 16 clients). **TCP is inbound-only in v1** —
  outbound feedback (§9) is UDP. Documented deviation; TCP send is a later add.
- The whole surface is gated by `oscEnabled` (default off). When off, no socket
  is bound and no feedback is sent.
- Host filtering: when `oscAcceptAnyHost` is **true** (default), any sender IP
  is accepted — required because trackers are typically not configured as send
  targets. When **false**, the allow-list is `{ loopback, oscSendAddress }`.

---

## 2. Message forms

Two equivalent forms are accepted **inbound**; the indexed form is the only one
used **outbound** (read-back and feedback).

- **Write form** (compact, stream-friendly):
  `/xoa/<family>/<param>` with the **channel as the first `int32` argument**
  (1-based), value(s) following. Example: `/xoa/input/gain (i)3 (f)-6.0`.
  Channel-less families (config, rotation, listener) carry only the value.
- **Indexed form** (addressable, OSCQuery-shaped for a future upgrade):
  `/xoa/<family>/<n>/<param>` with the channel **in the path** (1-based),
  value(s) only. Example: `/xoa/input/3/gain (f)-6.0`.

Channels are **1-based on the wire**; the app maps to its 0-based internal
index. Per-band speaker EQ is **indexed-form only** (§7, family
`/xoa/speaker/<n>/eq/<b>/<param>`) — a two-index target cannot ride the
single-channel inbound coalescer without dropping concurrent band writes.

---

## 3. Type coercion and validation

- `int32` and `float32` are accepted interchangeably for numeric leaves
  (tolerant extraction). Booleans are `int`/`float` 0 or 1. Strings are `s`.
- Messages containing any **NaN or ±Inf** float are rejected wholesale and
  logged; no partial application.
- Out-of-range values are **clamped, not rejected** (the store's write
  interceptor runs `constraints::clampToBounds`), except structural counts
  (§ config) which resize within `[1, max]`. A write that would exceed a
  bound lands at the bound and keeps the parameter live — it never reverts to
  the prior value.
- Unknown addresses are ignored (logged), not errors.

---

## 4. Coalescing, origin, and undo

- **Inbound**: the spatcore `OSCIngestQueue` coalesces newest-wins per
  `(address, channel)` and drains on the message thread at ~62.5 Hz (16 ms).
  A faster stream (e.g. 200 Hz tracker) collapses to one store write per
  address per tick. The indexed form bypasses channel-keyed coalescing
  (distinct paths), so rapid indexed writes to different channels never merge.
- **Outbound**: the spatcore `OSCRateLimiter` coalesces per `address:channel`
  and flushes at **50 Hz**.
- **Origin / undo**: inbound parameter writes carry `OriginTag::OSC`;
  head-tracking / tracked-position writes carry `OriginTag::Tracking`. **All
  OSC-originated writes bypass the undo history** (`setParameterWithoutUndo`) —
  a continuous control stream must not flood the per-domain `UndoManager`s.
  This is a deliberate divergence from the WFS-DIY convention (which ramps some
  OSC writes through undo). UI and file-load edits remain undoable as before.

---

## 5. Latency (the head-tracker caveat)

Worst-case write-to-audible latency for a rotation/position stream is the
inbound drain tick (≤ 16 ms) plus the engine response (store write →
`RtSnapshot::publish` → RT acquires next block → one-block matrix lerp =
≤ 2 buffers). At 512 samples / 48 kHz that is roughly `16 + 21 ≈ 37 ms`. This
is **playback-grade, not VR-grade** (PRD §9): fine for re-centering a mix or a
slow head-tracked listening session, not for low-latency head-locked VR audio.

---

## 6. Parameter families (write form)

Channel `(i)` is the first argument in the write form, or the `<n>` path
segment in the indexed form. `(T)` = the leaf's native type.

### `/xoa/input/<param>` `(i)ch (T)v` — per mono input (FR-5/FR-6)

| Param | Type | Range / meaning | id |
|---|---|---|---|
| `gain` | f | dB, [-60, 12] | `inputGain` |
| `mute` | i | 0/1 | `inputMute` |
| `name` | s | — | `inputName` |
| `positionX` `positionY` `positionZ` | f | meters, [-100, 100] | `inputPosition{X,Y,Z}` |
| `coordinateMode` | i | 0 cart / 1 cyl / 2 spherical (display only; storage is cartesian) | `inputCoordinateMode` |
| `maxSpeed` | f | m/s, [0, 20] (0 = off) | `inputMaxSpeed` |
| `trackingSmooth` | f | %, [0, 100] (1-Euro) | `inputTrackingSmooth` |
| `spread` | f | degrees, [0, 180] | `inputSpread` |
| `nfcEnabled` | i | 0/1 | `inputNfcEnabled` |

Note: `positionX/Y/Z` here are **direct parameter writes** (unconditioned). A
smoothed tracker stream uses `/xoa/tracking/position` (§8) instead.

### `/xoa/speaker/<param>` `(i)ch (T)v` — per output (FR-15)

| Param | Type | Range / meaning | id |
|---|---|---|---|
| `gain` | f | dB trim, [-60, 12] | `speakerGain` |
| `delay` | f | ms, [0, 500] | `speakerDelay` |
| `mute` | i | 0/1 | `speakerMute` |
| `solo` | i | 0/1 (transient) | `speakerSolo` |
| `name` | s | — | `speakerName` |
| `positionX` `positionY` `positionZ` | f | meters, [-100, 100] | `speakerPosition{X,Y,Z}` |
| `coordinateMode` | i | 0/1/2 (display only) | `speakerCoordinateMode` |
| `eqEnabled` | i | 0/1 | `speakerEqEnabled` |

### `/xoa/speaker/<n>/eq/<b>/<param>` `(T)v` — **indexed form only** (FR-15)

`<n>` = speaker (1-based), `<b>` = band 1–6.

| Param | Type | Range / meaning | id |
|---|---|---|---|
| `shape` | i | 0 OFF, 1 LowCut, 2 LowShelf, 3 Peak, 4 BandPass, 5 HighShelf, 6 HighCut, 7 AllPass | `eqShape` |
| `frequency` | f | Hz, [20, 20000] | `eqFrequency` |
| `gain` | f | dB, [-24, 24] | `eqGain` |
| `q` | f | [0.1, 10] | `eqQ` |
| `slope` | f | RBJ shelf S, [0.1, 1.0] | `eqSlope` |

### `/xoa/decoder/<param>` `(T)v` — decoder (FR-12/FR-13/FR-14)

| Param | Type | Range / meaning | id |
|---|---|---|---|
| `type` | i | 0 SAD / 1 mode-match / 2 AllRAD | `decoderType` |
| `weighting` | i | 0 basic / 1 max-rE | `decoderWeighting` |
| `dualBandEnabled` | i | 0/1 | `decoderDualBandEnabled` |
| `crossoverFrequency` | f | Hz, [80, 2000] | `decoderCrossoverFrequency` |
| `normalization` | i | 0 amplitude / 1 energy | `decoderNormalization` |

### `/xoa/config/<param>` `(T)v` — global (no channel)

| Param | Type | Range / meaning | id |
|---|---|---|---|
| `masterGain` | f | dB, [-60, 12] | `masterGain` |
| `distanceCompMode` | i | 0 off / 1 delay / 2 delay+gain | `distanceCompMode` |
| `monoInputsEnabled` | i | 0/1 | `monoInputsEnabled` |
| `playbackLoop` | i | 0/1 | `playbackLoop` |
| `playbackContentOrder` | i | 0 auto … 10 | `playbackContentOrder` |
| `playbackConvention` | i | 0 SN3D / 1 N3D / 2 FuMa | `playbackConvention` |
| `inputCount` | i | [1, max] — structural resize | `inputCount` |
| `speakerCount` | i | [1, max] — structural resize | `speakerCount` |

**Transport parameters are read-only over OSC** (`oscEnabled`,
`oscReceivePort`, `oscSendPort`, `oscSendAddress`, `oscTcpEnabled`,
`oscTcpPort`, `oscAcceptAnyHost`, `oscFeedbackEnabled`, `oscMeterEnabled`):
they are reportable via `/xoa/get` but a remote peer may not reconfigure the
transport out from under itself. `playbackFilePath` and transport play-state
are likewise not OSC-writable in v1.

### `/xoa/rotation/<param>` `(f)deg` — scene orientation (FR-9)

| Param | Range | id |
|---|---|---|
| `yaw` | degrees, [-180, 180] | `rotationYaw` |
| `pitch` | degrees, [-90, 90] | `rotationPitch` |
| `roll` | degrees, [-180, 180] | `rotationRoll` |

Convention: intrinsic **Z-Y'-X''** (yaw about +Z, then pitch, then roll), the
pinned WP4 convention (`Source/DSP/AmbiRotation.h`). `yaw+` turns the scene
toward the LEFT (+Y).

### `/xoa/listener/<param>` `(f)m` — listener sweet-spot position (D18 / FR-25)

| Param | Range | id |
|---|---|---|
| `x` `y` `z` | meters, [-100, 100] | `listener{X,Y,Z}` |

The listener position re-references the per-speaker distance compensation
(§FR-15): delays and gains are computed from ‖speaker − listener‖ instead of
‖speaker − origin‖, shifting the time/level sweet spot. **Time/level only** —
the decode matrix is unchanged (decoder redesign from the listener position is
post-v1). Default `(0, 0, 0)` = rig origin, at which the compensation is
bit-identical to the pre-D18 behaviour.

---

## 7. Head-tracking and tracked streams

### `/xoa/tracking/quaternion` `(f)w (f)x (f)y (f)z`

Head orientation as a unit quaternion (w-first; normalization not required —
the app normalizes). The field is **counter-rotated** (the inverse orientation
is applied) so that turning the listener's head right swings the scene left,
keeping sources world-stable. On receipt the quaternion is converted to the
`rotationYaw/Pitch/Roll` triple (Z-Y'-X'' decomposition) and written under
`OriginTag::Tracking`; the UI dials therefore follow the tracker, and read-back
is free. See §5 for the latency caveat.

> Substitution note (recorded in the DEVPLAN closeout): the DEVPLAN text says
> "quaternion → TrackingIngestQueue", but spatcore's `TrackingUpdate` POD
> carries only a yaw float, and spatcore is no-touch. The quaternion therefore
> rides the **main** `OSCIngestQueue` (newest-wins per address — the correct
> coalescing for a pose stream) and is decomposed on drain.

### `/xoa/tracking/position` `(i)input (f)x (f)y (f)z [(f)quality]`

A tracked **source** position for input `input` (1-based), routed through the
WP8 conditioning seam (`AmbiCalculationEngine::submitTrackedPosition`:
`InputSpeedLimiter` + 1-Euro `TrackingPositionFilter`, per-input `maxSpeed` /
`trackingSmooth`). Optional `quality` in [0, 1] biases the filter. Use this for
live tracker streams; use `/xoa/input/positionX` for direct parameter control.

### `/xoa/tracking/listener` `(f)x (f)y (f)z [(f)quality]` — **reserved (stretch)**

A conditioned continuous **listener** position stream (1-Euro smoothed →
`listenerX/Y/Z`). Address reserved and frozen here; the streaming
implementation is a cuttable WP9 stretch. Static listener control is always
available via `/xoa/listener/x|y|z`.

---

## 8. Query and handshake

| Address | Args | Reply (to sender's IP : source port) |
|---|---|---|
| `/xoa/get` | `(s)address [(i)ch]` | the addressed value in **indexed form**, e.g. request `/xoa/get "/xoa/input/positionX" 3` → reply `/xoa/input/3/positionX (f)v`. Unknown/unset address → no reply (logged). |
| `/xoa/ping` | `[(i)token]` | `/xoa/pong [(i)token]` — bypasses the outbound rate limiter; used by the control-replay harness for readiness. |

`/xoa/get` is the definition of "readable over OSC" and needs no target
configuration (it answers the requesting socket). It is what
`tools/validation/control-replay/osc_replay.py` asserts against.

---

## 9. Output streams (feedback and monitoring)

- **Parameter feedback** (gated by `oscFeedbackEnabled`, default on): every
  parameter change **except** those originated by `OriginTag::OSC` (echo-loop
  suppression) is emitted to the configured target in **indexed form**,
  rate-limited to 50 Hz. UI, file-load, and tracking-originated changes are fed
  back; a remote peer's own writes are not echoed to it.
- **Monitoring** (gated by `oscMeterEnabled`, default off), pushed at 10 Hz:

  | Address | Args |
  |---|---|
  | `/xoa/monitor/output/peak` | `(i)ch (f)linear` per output |
  | `/xoa/monitor/cpu` | `(f)load` (0–1) |
  | `/xoa/monitor/latencyMs` | `(f)ms` |

---

## 10. Config parameters added by WP9

New `Config` leaves (all route to Config scope by the `getParameterScope`
prefix rule; none is OSC-writable except as noted):

| id | Type | Default | Purpose |
|---|---|---|---|
| `listenerX` `listenerY` `listenerZ` | double (m) | 0, 0, 0 | D18 sweet-spot (OSC-writable via `/xoa/listener`) |
| `oscTcpEnabled` | bool | false | bind the TCP receiver |
| `oscTcpPort` | int | 9002 | TCP receive port |
| `oscAcceptAnyHost` | bool | true | disable IP allow-list |
| `oscFeedbackEnabled` | bool | true | emit parameter feedback |
| `oscMeterEnabled` | bool | false | emit `/xoa/monitor/*` |

---

## 11. Out of scope for v1 (decision D3)

- **Tracker profiles** — PSN, RTTrP/RTTrPM, MQTT. Only the generic OSC
  quaternion and generic tracked-position addresses above ship. (Post-v1
  backlog; the reserved `/xoa/tracking/*` namespace absorbs them.)
- **OSCQuery** — no HTTP namespace server, no `?VALUE|TYPE|RANGE` introspection,
  no WebSocket `LISTEN`. The indexed address form is deliberately OSCQuery-
  shaped so a later server maps 1:1. `/xoa/get` is the v1 read-back stand-in.
- **Multi-target output** — the architecture carries 6 target slots (spatcore
  `MAX_TARGETS`), but only target 0 (`oscSendAddress:oscSendPort`) is wired
  from the schema in v1. Multi-target config is WP10/WP12.
- **Zeroconf / service discovery.**
