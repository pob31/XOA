# Follow-ups

## Spatcore extraction candidates discovered during WP9 (2026-07-12)

While building the WP9 OSC control plane, several helpers landed **app-local in
XOA** that turn out to duplicate equivalent code in WFS-DIY. They are candidates
to promote into **spatcore as new, additive functions** (no existing signature
changes), so every consumer (XOA, WFS-DIY, "Tight-WFS", …) shares one copy.

Verified before listing:
- None of these exist in spatcore today (checked `spatcore/control/osc/`,
  `spatcore/control/state/TreeParameterStore.h`, `spatcore/dsp/`).
- WFS-DIY re-implements the same logic (`Source/Network/OSCMessageRouter.{h,cpp}`
  has `hasOnlyFiniteFloats`, `extractFloat`/`extractInt`; `OSCManager.cpp`
  `dispatchIngestedItem` has the `#bundle` decode; `TrackingRTTrPReceiver` has
  `quaternionToYaw`).

Process: authored **in the spatcore repo** (never from XOA, per the no-touch
rule in CLAUDE.md), then XOA and WFS drop their local copies and pick them up
via a pin bump. Budget like the other spatcore extractions.

### Tier 1 — clear wins (duplicated in both apps, app-agnostic, small)

1. **OSC wire + validation helpers** → `spatcore/control/osc/` (next to the
   existing `OSCParser` / `OSCSerializer`):
   - `parsePacket(bytes, size) → std::vector<OSCMessage>` — the
     `#bundle`-vs-message decode wrapper with `OSCFormatError` handling.
     - XOA: inline in `Source/Network/OSCManager.cpp::onDatagram`.
     - WFS: `Source/Network/OSCManager.cpp::dispatchIngestedItem`.
   - `hasOnlyFiniteFloats(msg, reason&)` — NaN/Inf gate.
     - XOA: `Source/Network/OSCMessageRouter.h`. WFS: `OSCMessageRouter`.
   - `extractNumber(arg, double&)` — tolerant int32↔float32 read.
     - XOA: `argToNumber` in `OSCMessageRouter.h`. WFS: `extractFloat`/`extractInt`.
   - `makeMessage(address, type, var)` — typed-var → OSC message builder.
     - XOA: `makeValueMessage` in `OSCMessageRouter.h`.
   - Highest ROI: tiny, pure, most-copied. Consumers share one NaN gate and one
     tolerant extractor instead of drifting.

2. **Global write-observer on `TreeParameterStore`** → the shared base class:
   - Additive `addWriteObserver(fn(id, value, channel, OriginTag))`, fired right
     after the existing `handlePostWrite`, before `notifyParameterListeners`.
   - Both apps need "watch every write + its origin to emit OSC/OSCQuery
     feedback." WP9 bolted a single `PostWriteObserver` onto the XOA subclass
     (`Source/Parameters/XoaValueTreeState.{h,cpp}`) because the base only offers
     the one `handlePostWrite` virtual (also wanted for invariants). Putting the
     observer in the base removes the per-app bolt-on.

### Tier 2 — plausible, with a caveat

3. **`quaternionToYawPitchRoll` / `matrixToEulerZYX`** → `spatcore/dsp/`:
   - The general form of WFS's `quaternionToYaw` (which only pulls yaw). My new
     `matrixToYawPitchRoll` in `Source/DSP/AmbiRotation.h` does the full 3-axis
     decomposition with a gimbal-pole branch.
   - Caveats: (a) frame / axis-order / sign conventions must be unified first
     (XOA's pinned intrinsic Z-Y'-X'' vs WFS's tracker frame) — a decision, not
     a copy-paste; (b) it lives in `AmbiRotation.h`, already slated for wholesale
     `spatcore/ambi/` extraction post-v1, so it may be cleaner to let it ride
     that later extraction than to pull it out early.

### Not candidates (by design)

- The `bindings()` address↔id table and the manager's routing/dispatch — app
  *dialect*; spatcore deliberately keeps the ingest classifier + dispatch
  app-injected.
- `setEqBandParameterWithoutUndo` — XOA schema-shaped.
- `tools/validation/control-replay/*` (osc_replay.py, osc_lib.py) — per-app
  tooling.

### Recommendation

Scope a first spatcore PR to **Tier 1 only** (the OSC helpers + the base-class
write-observer), authored in the spatcore working tree with its own tests.
Defer #3 to the `spatcore/ambi/` extraction. Decide separately whether to
refactor XOA + WFS onto the new helpers in the same pass or at the pin bump.

The win is deduplication + consistency across consumers, not code volume — worth
it once there is a third consumer; still reasonable with just XOA + WFS.
