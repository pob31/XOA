# control-replay — OSC replay harness (WP9)

`osc_replay.py` drives the **real XOA app** over OSC and checks that every
runtime-parameter family is writable and readable per `Documentation/XOA-OSC-MAP.md`.

It launches `XOA --osc` (the headless entry point: `--osc` force-enables the OSC
receiver on the default port 9000), waits for a `/xoa/ping` → `/xoa/pong`
readiness handshake, writes a deterministic set of values across every address
family, reads each back with `/xoa/get`, and diffs the result against
`goldens/osc_replay.json`. It also asserts hard invariants independent of the
golden: an out-of-range write **clamps** (does not revert), and transport
parameters (`oscReceivePort`, …) are **read-only over OSC**.

## Why it is not a unit test

It needs a built `XOA` binary and a running app, so it lives outside the
dependency-free `xoa-tests` suite (which never opens a device or a socket). It
is a **local / CI gate**. CI runs it on the Linux job under `xvfb-run` (XOA is a
GUI app); the Windows and macOS jobs stay unit-test-only. The app tolerates a
failed audio-device open on a headless runner — the store and OSC path are
independent of the device.

## Running

```
python3 tools/validation/control-replay/osc_replay.py            # check vs golden
python3 tools/validation/control-replay/osc_replay.py --update   # regenerate golden
python3 tools/validation/control-replay/osc_replay.py --exe path/to/XOA
```

Exit codes: `0` ok, `1` mismatch / invariant failure, `2` usage,
`3` app failed to start (no `/xoa/pong` before `--timeout`).

## Files

- `osc_replay.py` — the driver.
- `osc_lib.py` — a dependency-free OSC 1.0 codec (int32/float32/string only).
- `goldens/osc_replay.json` — the committed expected read-back values.

## Notes

- The harness kills stale `XOA` processes before launching (XOA is
  single-instance; a survivor would swallow the launch).
- On Windows it disables `SIO_UDP_CONNRESET` and tolerates `ConnectionResetError`
  so an ICMP "port unreachable" during startup does not poison the receive path.
- All golden values are binary-exact in float32 (multiples of 0.25 / integers),
  so the round-trip through the wire is stable.
