# XOA — Audio Device Layer & Patch Window Handoff

Version 0.1 — August 2026 — GPL-3.0

Handoff for the session that migrates XOA off its hand-rolled device handling and onto
`spatcore::io`, and later onto the Audio Interface window and patch matrix lifted from WFS-DIY.
Written 2026-08-02, immediately after the device layer landed on spatcore `main`.

Authority: this document owns the **how** of the migration. Where it conflicts with
`XOA-DEVPLAN.md` on ordering, the DEVPLAN wins — nothing here is on the WP critical path, and the
device layer is additive.

---

## 1. Status and what is available today

| Piece | Where | State |
|---|---|---|
| `spatcore::io` device layer | `spatcore/io/` on spatcore `main` | **AVAILABLE** — merged as `8e0d7e6` (PR #5), CI green on Windows/macOS/Linux |
| WFS-DIY consuming it | `feat/patch-diagnostics-512ch` in `d:/dev/WFS_DIY_v1` | **DONE, not yet merged** — the reference wiring to copy |
| Audio Interface window + patch matrix in `spatcore/ui` | — | **NOT STARTED** (Stage 2). Still app-side in WFS-DIY |

So this is a **two-stage** migration, and stage 1 does not depend on stage 2:

- **Stage 1 — adopt the device layer.** Available now. Replaces `AudioEngine`'s device open/close
  and its raw `AudioIODeviceCallback` with `spatcore::io::DeviceHost` + `DeviceIoCallback`, and
  converges the test-signal generator. Sections 3–6.
- **Stage 2 — adopt the window.** Blocked on the lift. XOA gains the full Audio Interface window
  and patch matrix in place of its `juce::AudioDeviceSelectorComponent`. Sections 7–8.

Pin target for stage 1:

```
git -C spatcore fetch origin
git -C spatcore checkout 8e0d7e6      # or origin/main if later work has landed
git add spatcore
```

XOA has no `bump-spatcore` script (WFS-DIY's `tools/bump-spatcore.ps1` is WFS-specific — its gate
list names kernel hashes, GPU plugin rebuilds and the prebuilt `wfs_hip.dll`, none of which XOA
has). The literal commands above are the whole ritual.

Two places record the pin in prose and must move with it: `CLAUDE.md` (currently `8296f28`,
correct for the submodule as it stands) and `Documentation/XOA-PLAN.md` §4, which still says
`spatcore @bf96b3c` and is two bumps stale. Update both to `8e0d7e6` in the bump commit.

---

## 2. What the device layer is, and why XOA needs it

It was written to fix two defects found in WFS-DIY. **XOA has one and a half of them.**

### 2.1 The 128-channel cap — XOA is NOT affected

`juce::AudioSourcePlayer` — the callback behind `juce::AudioAppComponent` — holds
`float* channels[128]` and stops compacting once those arrays are full, so a hardware channel at
index 128 or above simply has no slot in the buffer. WFS-DIY hit this because it is an
`AudioAppComponent`. **XOA is a raw `juce::AudioIODeviceCallback`** (`Source/Audio/AudioEngine.h:45`)
and never had the cap. Nothing to fix here; it is listed so nobody re-derives the fear.

### 2.2 Channel masks that do not stick — XOA IS affected

`juce::AudioDeviceManager` keeps `useDefaultInputChannels` / `useDefaultOutputChannels`, both
defaulting to **true**. While either is set, `setAudioDeviceSetup()` **discards the caller's
`BigInteger` mask** and substitutes `range(0, numChansNeeded)` — a count frozen at whatever the
last `initialise()` asked for. Two consequences for XOA:

- XOA opens with `deviceManager.initialise (xoa::kMaxInputs /*64*/, xoa::kMaxSpeakers /*256*/,
  savedXml.get(), true)` (`Source/Audio/AudioEngine.cpp:109`) and never calls
  `setAudioDeviceSetup` anywhere — `grep -rn "setAudioDeviceSetup|useDefaultInput" Source/` returns
  nothing. So the mask is always `range(0,64)` / `range(0,256)`, and a **>256-output rig is capped
  at 256** with no diagnostic.
- Because the flags stay true, `createStateXml()` **never writes `audioDeviceInChans` /
  `audioDeviceOutChans`**. XOA persists `ids::audioDeviceState` on every device change
  (`AudioEngine.cpp:337`) but that state cannot carry a channel selection, so it cannot round-trip
  one. Today that is invisible because XOA offers no channel selection; the moment the patch window
  arrives in stage 2 it becomes a data-loss bug.

`spatcore::io::DeviceHost` exists to enforce exactly this: **every** mutation writes an explicit
mask with both flags cleared.

### 2.3 Compacted indices versus hardware channel numbers — XOA IS affected, latently

This is the half. The device callback arrays contain only the **enabled** channels, packed with no
holes: entry *k* is the *k*-th set bit of the mask, not hardware channel *k*. XOA identity-maps
throughout — stem *i* ← `inputChannelData[i]` (`AudioEngine.cpp:435-442`), speaker *s* →
`outputChannelData[s]` via the `outBuf` wrapper (`:397`) — and has **no patch or routing concept at
all**. That identity is correct only while the mask is contiguous from bit 0.

It is contiguous today, because XOA only ever asks for `range(0, N)`. It stops being contiguous the
moment anything can deselect a channel — which is precisely what stage 2 adds. `HardwareIndexMap`
is the translation that makes hardware numbering true by construction; `DeviceIoCallback` applies it
so the buffer row index **is** the hardware channel number.

Related: `AudioEngine.cpp:363` derives the speaker count as
`device->getActiveOutputChannels().countNumberOfSetBits()` — a **count**, which is passed to
`algorithm.prepare` and `speakerComp.prepare`. Count and highest-index agree only for a contiguous
mask. See §5.3.

---

## 3. Stage 1 — the API to adopt

Four header-only files, namespace `spatcore::io`, target `spatcore-io` (option `SPATCORE_IO`,
default ON). Read the headers; they carry the rationale inline.

| Header | Purpose |
|---|---|
| `spatcore/io/HardwareIndexMap.h` | `fromMasks(activeIn, activeOut, maxChannels)` → `numChannels`, `inputIndexForHw[]`, `outputIndexForHw[]` (`-1` = that direction is disabled on that hardware channel), `isIdentityMapping()`. `juce_core` only, fully unit-tested. |
| `spatcore/io/DeviceIoCallback.h` | `juce::AudioIODeviceCallback` driving a `juce::AudioSource`, **no channel cap**, buffer indexed by hardware channel. Sizes everything in `audioDeviceAboutToStart`; the callback allocates nothing and takes no lock. |
| `spatcore/io/DeviceHost.h` | Open/restore policy over a **non-owned** `juce::AudioDeviceManager&`. `restoreFromXml`, `openNamedDevice`, `setDeviceAllChannels`, `enableAllChannels`, plus the truthful `getNumActiveInputs/Outputs` and `getActiveInput/OutputMask`. |
| `spatcore/io/TestSignalGenerator.h` | Off / PinkNoise / Tone / Sweep / DiracPulse, replace-semantics, **500 ms protective ramp**. |

Build change — one line added to the existing `target_link_libraries` call at
`CMakeLists.txt:104-113`. The whole call, so nothing gets dropped by a paste:

```cmake
target_link_libraries(XOA PRIVATE
    spatcore-audio
    spatcore-control
    spatcore-controllers
    spatcore-ui
    spatcore-io                    # <- add
    hidapi::hidapi
    juce::juce_audio_utils
    juce::juce_gui_extra
    juce::juce_opengl
    juce::juce_osc)
```

`spatcore-io` links `juce_audio_devices`, which pulls in the platform driver backends
(ASIO/WASAPI, CoreAudio, ALSA/JACK). That is why it is a separate target from `spatcore-audio` —
the audio layer must stay linkable where the host owns the device. XOA opens its own device, so it
wants it.

### 3.1 The reference wiring

WFS-DIY's is the wiring to copy. **Paths in this table are in the WFS-DIY checkout**
(`d:/dev/WFS_DIY_v1`, branch `feat/patch-diagnostics-512ch`) — everywhere else in this document, a
bare `Source/...` path means XOA:

| What | Where |
|---|---|
| Member declarations | `d:/dev/WFS_DIY_v1/Source/MainComponent.h`, next to `audioCallbacksAttached` |
| Startup restore, saved state | `Source/MainComponent.cpp` — `deviceHost.restoreFromXml(savedStateXml.get(), false)` |
| Startup restore, by name | `deviceHost.openNamedDevice(savedDeviceType, savedDeviceName)` |
| Callback registration | `attachAudioCallbacksIfNeeded()` (`:2434`) — `deviceManager.addAudioCallback(&ioCallback)` (`:2451`), once |
| Teardown | destructor: `removeAudioCallback` **before** closing the device |
| Truthful channel counts to the UI | `changeListenerCallback` and `loadAudioPatches` |
| Device picker | `d:/dev/WFS_DIY_v1/Source/gui/AudioInterfaceWindow.cpp` — `DeviceSettingsPanel::deviceChanged` / `enableAllChannels` |

Two details that are easy to get wrong:

- **Register the callback once.** `AudioDeviceManager` re-arms every registered callback across
  device changes, so there is nothing to re-attach later. XOA already does this
  (`AudioEngine.cpp:111`); keep it.
- **`removeAudioCallback` blocks until the audio thread has left the callback.** It must run before
  anything the callback touches is destroyed. XOA's `closeAudioDevice()` already has the right
  order (`AudioEngine.cpp:121-133`).

---

## 4. Stage 1 — migration steps

1. **Bump the pin and link the target** (§1, §3). Build. Nothing else changes yet; this commit is
   green on its own.
2. **Replace the test-signal generator.** See §5.4 — this is the one step with a real behavioural
   decision in it, and it is independent of the rest, so it can land first or last.
3. **Wrap the device manager in `DeviceHost`.** `AudioEngine` owns `juce::AudioDeviceManager` by
   value (`AudioEngine.h:201`) and `DeviceHost` takes a reference, so this is additive — no
   ownership moves:

   ```cpp
   spatcore::io::DeviceHost deviceHost { deviceManager, 512 };
   ```

   Then `openAudioDevice()` becomes `deviceHost.restoreFromXml (savedXml.get(), true)` in place of
   the bare `deviceManager.initialise(...)`. That single substitution buys the explicit-mask policy,
   the enable-all-channels behaviour and the >256-channel ceiling.

   Pick XOA's own name for the `512`. WFS-DIY passes
   `WFSValueTreeState::maxHardwarePatchChannels`; XOA should add an equivalent to
   `Source/XoaConstants.h` rather than relying on the default, so the policy number has one home.
   It is a **ceiling, not an allocation** — `applyEnableAllPolicy` builds each mask from the
   device's real channel counts and only clamps to it, so passing 512 for a rig with 8 outputs
   costs nothing.

4. **Convert `AudioEngine` to a `juce::AudioSource` and let `DeviceIoCallback` drive it.** Less work
   than it sounds: the callback body is already `getNextAudioBlock`-shaped — it wraps the outputs in
   a `juce::AudioBuffer` (`AudioEngine.cpp:397`) and builds a
   `juce::AudioSourceChannelInfo` (`:409`). The three overrides map one-for-one:

   | Today | Becomes |
   |---|---|
   | `audioDeviceIOCallbackWithContext(...)` | `getNextAudioBlock (const juce::AudioSourceChannelInfo&)` |
   | `audioDeviceAboutToStart(device)` (`:346`) | `prepareToPlay (samplesPerBlockExpected, sampleRate)` |
   | `audioDeviceStopped()` (`:378`) | `releaseResources()` |

   `prepareToPlay` receives only block size and sample rate, so anything `audioDeviceAboutToStart`
   currently reads off the device must come from elsewhere — `getOutputLatencyInSamples()` for
   `measuredLatencyMs` (`:350` and the `.store` at `:356`) and `getActiveOutputChannels()` for
   `numOut` (`:363`). Both
   are available through `DeviceHost` (`getNumActiveOutputs()`) or by keeping the
   `ChangeListener` XOA already has on the device manager. **Do not** re-derive them from the
   buffer width — see §5.1.

   Then register `DeviceIoCallback` instead of `this`:

   ```cpp
   spatcore::io::DeviceIoCallback ioCallback { *this, 512 };
   ...
   deviceManager.addAudioCallback (&ioCallback);
   ```

5. **Re-point the stem gather and the speaker write at the hardware-indexed buffer** — §5.1 and
   §5.2. This is the step that can silently corrupt audio; read those two sections before writing
   any of it.
6. **Run the acceptance pass** (§6). CI cannot cover any of this.

---

## 5. Hazards — read before writing code

### 5.1 One buffer row is BOTH hardware input *h* and hardware output *h*

`DeviceIoCallback` gives the source a single buffer where row *h* is hardware channel *h*. For a
channel that is an active output, that row **aliases the device's own output storage**, and the
matching input is copied **into that same row** before the source runs
(`spatcore/io/DeviceIoCallback.h`). This is not new — `AudioSourcePlayer` has always done it — but
XOA has never seen it, because today its inputs and outputs live in two separate arrays.

The invariant this creates:

> **Every input must be read before anything writes an output.**

XOA satisfies this today by accident of ordering: stems are gathered into `stemScratch`
(`AudioEngine.cpp:435-442`) before `algorithm.processBlock` writes (`:459`/`:465`). After the
migration that ordering becomes **load-bearing** — reordering the gather below the decode would
silently feed the encoder its own decoder output on every channel where a speaker and a microphone
share a hardware index. State it as a comment at the gather site.

### 5.2 Buffer width is NOT the speaker count

The buffer spans `min(maxChannels, max(highest active input, highest active output) + 1)` hardware
channels — and 0 when both masks are empty. On a rig
with 64 inputs and 6 outputs the buffer is **64** channels wide. XOA currently derives everything
from `numOutputChannels` as handed to the callback; after migration, taking the speaker count from
`info.buffer->getNumChannels()` would run the decoder for 64 speakers and write rows 6–63, which are
input-only rows whose writes are discarded — wasted work, wrong meters, no error.

Take the speaker count from `DeviceHost::getNumActiveOutputs()` (or the map), never from the buffer.

### 5.3 `countNumberOfSetBits()` is a count, not an index bound

`AudioEngine.cpp:363` passes `getActiveOutputChannels().countNumberOfSetBits()` to
`algorithm.prepare` and `speakerComp.prepare`, and the meter arrays are `std::array<...,
kMaxSpeakers>` / `kMaxInputs` indexed by the same ordinal (`AudioEngine.h:223,226`, bounds-checked at
`:117-130`). Under hardware indexing, count and index diverge as soon as a mask has a hole.

Decide explicitly, and write the decision down as a `D<n>` record:

- **Option A — require contiguity.** `DeviceHost`'s enable-all policy always produces
  `range(0, N)`, so as long as nothing else writes a mask, count == index bound. Cheapest; breaks
  the day stage 2 lets an operator deselect a channel.
- **Option B — index by hardware channel.** Raise `kMaxSpeakers` to the addressing ceiling and index
  the meters by hardware channel. Honest, and what the patch window will want.
- **Option C — keep a compaction stage.** A speaker→hardware map of XOA's own, i.e. reinventing the
  patch. Only sensible if stage 2 is abandoned.

Recommendation: **A now, B when stage 2 lands**, with the assumption asserted
(`jassert (map.isIdentityMapping())`) so option A fails loudly instead of misrouting.

### 5.4 The shared `TestSignalGenerator` has no SpeakerId mode

`spatcore::io::TestSignalGenerator` is WFS-DIY's, with one fix: `prepare()` now recomputes the
tone's phase increment, which used to default to zero and be written only by `setFrequency()` — a
Tone selected on any path that skipped it was **pure silence**, and it came back at the wrong pitch
after a sample-rate change. It also gained `setDeterministicSeed()`.

Two XOA behaviours are **not** in it:

| XOA today | Shared version |
|---|---|
| `kSeed = 0x0A0A` always-on fixed seed (`Source/Audio/TestSignalGenerator.h:54`) | opt-in `setDeterministicSeed(42)`; default seeds from the wall clock |
| `SpeakerId` mode — declicked pink burst stepping across every output, `getCurrentSpeakerIndex()` (`:126-128`, `:165-204`) | absent |

The seed is a one-line call at `prepare()`. `SpeakerId` is a real feature and was deliberately left
out of the shared class because adding an enum member makes WFS-DIY's `switch` statements
non-exhaustive for a mode nothing there uses. Consumers that would break:
`Source/GUI/Tabs/SpeakersDecoderTab.cpp:169,312-317` and two checks in
`tests/XoaTestSignalTests.cpp`.

**Do this as a spatcore PR, not an XOA workaround.** Add `SignalType::SpeakerId` plus
`getCurrentSpeakerIndex()` to `spatcore/io/TestSignalGenerator.h`, port XOA's `renderSpeakerId`
verbatim, and add a test alongside the existing generator tests in
`spatcore/tests/SpatcoreTests.cpp`. Keeping a forked generator in XOA re-creates exactly the
duplication the shared layer exists to remove.

Adding an enum member makes WFS-DIY's `switch` statements over `SignalType` non-exhaustive, which
is why it was left out in the first place. Four sites in `d:/dev/WFS_DIY_v1` need the new case:
`Source/gui/AudioPatchTab.cpp:179`, `:473` and `:818` — all three have no `default:`, so they warn
(`-Wswitch` / MSVC C4062) — and `Source/gui/PatchMatrixComponent.cpp:1138` (also WFS-DIY), which does have a
`default:` and so compiles silently but would render the new type with the fallback colour. The
`switch` statements at `AudioPatchTab.cpp:151` and `:596` are over combo-box ids, not the enum, and
need nothing.

### 5.5 `DeviceHost` needs a scanned device manager

`AudioDeviceManager` only builds its device-type list inside `initialise()` and the getters that
call `scanDevicesIfNeeded()`. On an un-initialised manager, `setAudioDeviceSetup()` takes a silent
early exit that opens nothing and returns an **empty error string**. `DeviceHost` guards against
this internally (`ensureDeviceTypesScanned`, commit `1f746f6`) — it was found only by pointing the
real WFS-DIY code at a Dante Virtual Soundcard whose service was down. Do not re-introduce the
pattern by calling `setAudioDeviceSetup` directly.

---

## 6. Stage 1 acceptance — CI cannot do this on hardware

No CI anywhere exercises a real multi-channel interface. Do **not** read that as "CI never touches
the device layer", though — two of XOA's lanes do, and they are a useful free gate:

- XOA's `.github/workflows/ci.yml` runs three ctest lanes, which open no device, plus two Linux
  lanes that launch the real binary under `xvfb`: the OSC control-replay (`osc_replay.py` spawns
  `XOA --osc`) and `xvfb-run -a "$XOA_BIN" --gui-smoke`. `AppShell`'s constructor calls
  `engine.openAudioDevice()` **unconditionally** (`Source/App/AppShell.cpp:88`) *before*
  `applyStartupCommandLine` has even seen `--osc` / `--gui-smoke` (`:89`), and that reaches
  `deviceManager.initialise (kMaxInputs, kMaxSpeakers, savedXml.get(), true)` with
  `selectDefaultDeviceOnFailure = true`. There is no opt-out flag. So an assert, crash or hang
  introduced in `DeviceHost` or `DeviceIoCallback` at startup **will** fail both of those lanes —
  worth knowing when steps 3 and 4 of §4 land, and worth not mistaking for an unrelated CI flake.
- spatcore's CI builds `examples/minimal-app` on three OSes; it does not run `spatcore-tests`.
- `spatcore-tests` covers `HardwareIndexMap` (contiguous / sparse / empty / clamped),
  `DeviceHost::applyEnableAllPolicy`, and the generator's pitch, ramp and seeded reproducibility —
  but **never drives `DeviceIoCallback`**, because that needs a live `juce::AudioIODevice`
  (`spatcore/tests/SpatcoreTests.cpp`, see the comment above the io tests).

So the migration is proven by hand, on hardware:

1. **Every channel opens.** With the largest interface available, `getNumActiveOutputs()` equals the
   device's output count, and the saved `audioDeviceState` XML now contains
   `audioDeviceInChans` / `audioDeviceOutChans` attributes. Their presence is the proof that the
   `useDefault*Channels` fix took.
2. **The masks survive a restart.** Quit, relaunch, re-check the active counts.
3. **A high channel is reachable.** Send the test signal to the highest output and confirm it
   sounds. On a >128-channel rig this is the case that was structurally impossible before.
4. **Inputs still reach the encoder.** Feed a signal to hardware input 0 while a speaker is mapped
   to hardware output 0 and confirm the stem is clean — the §5.1 aliasing case.
5. **A failing device reports why.** Point it at a driver whose service is stopped; the error string
   must be non-empty (§5.5).
6. **Latency and meters unchanged.** `measuredLatencyMs`, `inputPeak`, `outputPeak` read as before.

A throwaway console harness is the fastest way to do 1, 3 and 5 without the GUI: a
`juce_add_console_app` that `add_subdirectory`s JUCE, includes the four `spatcore/io` headers, opens
a device through `DeviceHost` and prints listed-vs-active counts, `useDefault*` flags, the map width
and whether a tone rendered. That is how the `ensureDeviceTypesScanned` bug was caught. JUCE 9
bundles the ASIO SDK (`modules/juce_audio_devices/native/asio/iasiodrv.h`), so `JUCE_ASIO=1` needs
no external SDK and the harness can enumerate real ASIO drivers.

---

## 7. Stage 2 — the window XOA inherits

WFS-DIY's Audio Interface window replaces `juce::AudioDeviceSelectorComponent`
(`Source/GUI/Tabs/SystemConfigTab.cpp:66-70`, constructed `(mgr, 0, 64, 0, 256, false, false, false,
false)`). What arrives:

- **Device settings** — device type, device, sample rate, buffer size, with all channels enabled
  automatically on selection.
- **An input patch matrix and an output patch matrix** — a scrollable grid of app channels ×
  hardware channels, with Scrolling / Patching / Testing modes, drag-diagonal 1:1 patching, and
  per-hardware-input **signal-presence tinting** so an operator can see which physical input is
  live.
- **Test tones per hardware channel**, click or keyboard, with the 500 ms protective ramp.
- Keyboard navigation and TTS announcements throughout.

For XOA this is a capability gain, not a refactor: XOA has **no patch concept at all** today, so
adopting the window means adopting routing it never had. Speaker *s* stops being hardware output *s*
by assumption and becomes hardware output *patch(s)* by data. Budget for that — it touches
`algorithm.prepare`'s channel count, the meter indexing (§5.3) and anything that assumes speaker
ordinal equals device ordinal.

### 7.1 What XOA already has for the provider seams

The lifted window will be app-agnostic in the shared-EQ style: `std::function` providers read at use
time, raw `juce::ValueTree` + `juce::Identifier` for state, and the consuming app keeping a thin
shim header of `using` aliases plus a config factory. XOA has a near-identical counterpart for
almost every seam, which is why this lift is worth doing:

| Seam | WFS-DIY | XOA |
|---|---|---|
| Parameter store | `WFSValueTreeState` | `XoaValueTreeState` — both derive `spatcore::control::state::TreeParameterStore` |
| Colours | `ColorScheme::get()` + `ColorScheme::Manager` listener | same names |
| Strings | `LOC()` macro | same |
| Status line | `StatusBar` | same |
| Accessibility | `TTSManager` singleton | same |
| Long-press button | `LongPressButton` | same |
| Window helpers | `WindowUtils`, `HelpCardSVG` | `WindowUtils`, `HelpCard` |
| Stream Deck+ pages | `PatchWindowPages` wired in `MainComponent` | **absent** — XOA links `spatcore-controllers` but never includes it |

Two consequences worth stating up front:

- XOA inherits the window's Stream Deck accessor surface as **unused public API**. That is why the
  lift cannot trim it; do not treat those accessors as dead code.
- Localisation cost on the XOA side is small: the window uses ~32 distinct `audioPatch.*` keys, and
  XOA ships **two** locales (`Resources/lang/en.json`, `fr.json`, single tier) against WFS-DIY's
  nine across two tiers. ~64 string entries total.

### 7.2 Sequencing

1. WFS-DIY decouples the three GUI files and moves them to `spatcore/ui/patch/` — spatcore PR.
2. WFS-DIY re-pins and reduces its own files to shims — app PR. Behaviour parity is verified by hand
   there, not by XOA.
3. XOA adopts: re-pin, link, replace the device selector, add the patch ValueTree section to its
   schema, and re-point the decoder at patched hardware channels.

Do **not** start step 3 before step 2 has been through a real show or a full manual pass in WFS-DIY.
The GUI has no bit-exactness gate, so WFS-DIY's own use is the only regression net the lift gets.

---

## 8. Open decisions

Record each as a `D<n>` in `XOA-DEVPLAN.md` when taken. That file reaches **D34**, so **D35 is the
next free number** — reserve D35–D40 for the six below rather than minting numbers ad hoc, as D18
does.

1. **Does `AudioEngine` become a `juce::AudioSource`, or gain a thin adapter?** WFS-DIY kept
   `juce::AudioAppComponent` purely to inherit `deviceManager` and to be the `AudioSource` passed as
   `*this`, never calling `setAudioChannels()` so the base player stays inert. XOA has no such
   constraint and can implement `juce::AudioSource` cleanly — recommended.
2. **Meter and prepare indexing: option A, B or C of §5.3**, and when B takes over.
3. **XOA's addressing ceiling constant** — the name and home for the `512` (§4 step 3).
4. **Who owns `SpeakerId`** — the recommendation in §5.4 is spatcore, in a small PR of its own.
5. **Should `DeviceHost` grow `setSampleRate` / `setBufferSize` / `resetToSavedSetup`?** WFS-DIY's
   device panel still calls `setAudioDeviceSetup` directly at three sites in
   `d:/dev/WFS_DIY_v1/Source/gui/AudioInterfaceWindow.cpp` — `:147` (Reset Device, a full re-open
   from a saved setup, and so the one most exposed to the mask policy), `:516` (sample rate) and
   `:546` (buffer size). All three are mask-safe only because a prior `DeviceHost` call already
   cleared the `useDefault*` flags in the stored setup. Making that an enforced invariant rather
   than an assumed one is a small spatcore change, best made **before** the window is lifted.
6. **Does the io layer ship as a tag?** spatcore is tagged only `v0.1.0` / `v0.1.1` while its
   `CMakeLists.txt` declares `VERSION 0.2.0`. Pin a bare SHA (`8e0d7e6`) or tag `v0.2.0` first.

---

## 9. Reference

| Thing | Path |
|---|---|
| Device layer | `spatcore/io/{HardwareIndexMap,DeviceIoCallback,DeviceHost,TestSignalGenerator}.h` |
| Its tests | `spatcore/tests/SpatcoreTests.cpp` (io section, at the end) |
| CMake target | `spatcore/CMakeLists.txt`, the `SPATCORE_IO` block |
| Reference consumer | `d:/dev/WFS_DIY_v1`, branch `feat/patch-diagnostics-512ch` |
| The window to be lifted | `d:/dev/WFS_DIY_v1/Source/gui/{AudioInterfaceWindow,AudioPatchTab,PatchMatrixComponent}.{h,cpp}` |
| Shared-component precedent | spatcore `d7967f7` + WFS-DIY `e7ad1c7` (the EQ) |
| XOA's current device code | `Source/Audio/AudioEngine.{h,cpp}`, `Source/Audio/TestSignalGenerator.h`, `Source/GUI/Tabs/SystemConfigTab.cpp:66-70` |
