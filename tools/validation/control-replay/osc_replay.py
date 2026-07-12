#!/usr/bin/env python3
"""XOA OSC control-replay harness.

Launches the real XOA app with OSC enabled (headless entry point: `XOA --osc`),
waits for a /xoa/ping -> /xoa/pong readiness handshake, drives a deterministic
list of parameter writes across every address family, reads each back with
/xoa/get, and compares the result to a committed golden JSON. Also asserts a
few hard invariants (out-of-range clamp, transport params read-only over OSC)
that must hold regardless of the golden.

This is a LOCAL / CI gate: it needs a built XOA binary, so it is not part of the
unit-test suite. CI runs it on Linux under xvfb (the app is a GUI app). See the
DEVPLAN test-evolution table.

exit codes: 0 ok, 1 mismatch / invariant failure, 2 usage, 3 app failed to
            start (no /xoa/pong before timeout).
"""

import argparse
import json
import os
import platform
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import osc_lib

APP_PORT = 9000          # matches oscReceivePortDefault
GOLDEN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "goldens", "osc_replay.json")

# label, write-address, write-args, get-address, get-channel (None = no channel arg)
CASES = [
    ("masterGain",        "/xoa/config/masterGain",          [("f", -6.5)],         "/xoa/config/masterGain",          None),
    ("rotationYaw",       "/xoa/rotation/yaw",               [("f", 45.0)],         "/xoa/rotation/yaw",               None),
    ("rotationPitch",     "/xoa/rotation/pitch",             [("f", -30.0)],        "/xoa/rotation/pitch",             None),
    ("listenerX",         "/xoa/listener/x",                 [("f", 1.25)],         "/xoa/listener/x",                 None),
    ("listenerY",         "/xoa/listener/y",                 [("f", -0.5)],         "/xoa/listener/y",                 None),
    ("distanceCompMode",  "/xoa/config/distanceCompMode",    [("i", 2)],            "/xoa/config/distanceCompMode",    None),
    ("crossover",         "/xoa/decoder/crossoverFrequency", [("f", 500.0)],        "/xoa/decoder/crossoverFrequency", None),
    # write form (channel = first int arg); read back via write-form get + channel
    ("input1.positionX",  "/xoa/input/positionX",            [("i", 1), ("f", 2.5)], "/xoa/input/positionX",           1),
    # indexed write; write-form get
    ("input2.gain",       "/xoa/input/2/gain",               [("f", -9.0)],         "/xoa/input/gain",                 2),
    # write form set; indexed get
    ("speaker3.gain",     "/xoa/speaker/gain",               [("i", 3), ("f", -3.0)], "/xoa/speaker/3/gain",           None),
    ("speaker1.mute",     "/xoa/speaker/1/mute",             [("i", 1)],            "/xoa/speaker/1/mute",             None),
    ("speaker2.eq3.freq", "/xoa/speaker/2/eq/3/frequency",   [("f", 1200.0)],       "/xoa/speaker/2/eq/3/frequency",   None),
]


def make_socket():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if hasattr(socket, "SIO_UDP_CONNRESET"):   # Windows: don't let ICMP unreachable poison recv
        try:
            s.ioctl(socket.SIO_UDP_CONNRESET, False)
        except OSError:
            pass
    s.bind(("127.0.0.1", 0))
    s.settimeout(0.4)
    return s


def kill_stale(exe):
    """XOA is single-instance; a stale process would swallow our launch."""
    name = os.path.basename(exe)
    try:
        if platform.system() == "Windows":
            subprocess.run(["taskkill", "/F", "/IM", name], capture_output=True)
        else:
            subprocess.run(["pkill", "-f", name], capture_output=True)
    except Exception:
        pass
    time.sleep(1.5)


def send(sock, address, args=None):
    sock.sendto(osc_lib.encode(address, args or []), ("127.0.0.1", APP_PORT))


def recv_reply(sock, want_address, retries=6):
    for _ in range(retries):
        try:
            data, _ = sock.recvfrom(65535)
        except (socket.timeout, ConnectionResetError):
            continue
        addr, vals = osc_lib.decode(data)
        if addr == want_address:
            return vals
    return None


def wait_ready(sock, proc, timeout):
    deadline = time.time() + timeout
    token = 7
    while time.time() < deadline:
        if proc.poll() is not None:
            return False
        try:
            send(sock, "/xoa/ping", [("i", token)])
            data, _ = sock.recvfrom(65535)
            addr, vals = osc_lib.decode(data)
            if addr == "/xoa/pong" and vals == [token]:
                return True
        except (socket.timeout, ConnectionResetError):
            pass
        time.sleep(0.3)
    return False


def get_value(sock, get_addr, channel):
    args = [("s", get_addr)] + ([("i", channel)] if channel is not None else [])
    # The reply address is always the indexed form; derive it for matching.
    reply_addr = get_addr
    if channel is not None:
        parts = get_addr.split("/")            # /xoa/input/gain -> /xoa/input/<ch>/gain
        parts.insert(3, str(channel))
        reply_addr = "/".join(parts)
    send(sock, "/xoa/get", args)
    return recv_reply(sock, reply_addr)


def approx(a, b):
    return abs(float(a) - float(b)) < 1e-4


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", help="path to the XOA binary (auto-probed if omitted)")
    ap.add_argument("--update", action="store_true", help="rewrite the golden JSON")
    ap.add_argument("--timeout", type=float, default=30.0, help="readiness timeout (s)")
    ap.add_argument("--settle", type=float, default=0.12, help="delay between writes (s)")
    ap.add_argument("--keep", action="store_true", help="leave the app running on exit")
    args = ap.parse_args()

    exe = args.exe
    if not exe:
        # Probe both the Windows multi-config layout (XOA_artefacts/Release/XOA.exe)
        # and the Linux single-config layout (XOA_artefacts/XOA or .../Debug/XOA).
        root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
        artefacts = os.path.join(root, "build", "XOA_artefacts")
        found = []
        for dirpath, _, files in os.walk(artefacts):
            for name in ("XOA.exe", "XOA"):
                if name in files:
                    found.append(os.path.join(dirpath, name))
        # Prefer a Release build if several are present.
        found.sort(key=lambda p: (0 if "Release" in p else 1, len(p)))
        exe = found[0] if found else None
    if not exe or not os.path.isfile(exe):
        print("error: XOA binary not found; pass --exe", file=sys.stderr)
        return 2

    kill_stale(exe)
    sock = make_socket()
    proc = subprocess.Popen([exe, "--osc"])

    try:
        if not wait_ready(sock, proc, args.timeout):
            print("error: app did not answer /xoa/ping within %.0fs" % args.timeout, file=sys.stderr)
            return 3
        print("ready: /xoa/pong received")

        # Establish the fixture shape over OSC (8 speakers, 2 inputs).
        send(sock, "/xoa/config/speakerCount", [("i", 8)]); time.sleep(0.3)
        send(sock, "/xoa/config/inputCount",   [("i", 2)]); time.sleep(0.3)

        for _, waddr, wargs, _, _ in CASES:
            send(sock, waddr, wargs)
            time.sleep(args.settle)
        time.sleep(0.5)   # let the ingest queue drain

        results = {}
        problems = []
        for label, _, wargs, gaddr, gch in CASES:
            vals = get_value(sock, gaddr, gch)
            if not vals:
                problems.append("NO REPLY  " + label)
                continue
            results[label] = vals[0]

        # Hard invariants (independent of the golden).
        # 1. Out-of-range write clamps (masterGain max = 12 dB), does not revert.
        send(sock, "/xoa/config/masterGain", [("f", 999.0)]); time.sleep(0.4)
        clamped = get_value(sock, "/xoa/config/masterGain", None)
        if not clamped or not approx(clamped[0], 12.0):
            problems.append("INVARIANT clamp: masterGain 999 -> %s (want 12.0)" % clamped)

        # 2. Transport params are read-only over OSC: a bogus port write must not
        #    move the receiver (we still get a pong on APP_PORT afterwards).
        send(sock, "/xoa/config/oscReceivePort", [("i", 9100)]); time.sleep(0.4)
        if recv_reply_ping(sock) is False:
            problems.append("INVARIANT read-only: oscReceivePort write moved the receiver")

        if args.update:
            os.makedirs(os.path.dirname(GOLDEN), exist_ok=True)
            with open(GOLDEN, "w") as f:
                json.dump(results, f, indent=2, sort_keys=True)
                f.write("\n")
            print("wrote %s (%d entries)" % (GOLDEN, len(results)))
            return 0 if not problems else 1

        if not os.path.isfile(GOLDEN):
            print("error: golden %s missing - run with --update" % GOLDEN, file=sys.stderr)
            return 1
        with open(GOLDEN) as f:
            expected = json.load(f)

        for label in sorted(set(list(expected) + list(results))):
            if label not in results:
                problems.append("MISSING   " + label)
            elif label not in expected:
                problems.append("EXTRA     " + label + " = " + repr(results[label]))
            elif not approx(results[label], expected[label]):
                problems.append("MISMATCH  %s: got %r want %r" % (label, results[label], expected[label]))

        if problems:
            print("osc_replay FAILED:")
            for p in problems:
                print("  " + p)
            return 1

        print("osc_replay OK (%d values match %s)" % (len(results), os.path.basename(GOLDEN)))
        return 0
    finally:
        sock.close()
        if not args.keep:
            try:
                proc.terminate()
                proc.wait(timeout=5)
            except Exception:
                proc.kill()


def recv_reply_ping(sock):
    send(sock, "/xoa/ping", [("i", 99)])
    return recv_reply(sock, "/xoa/pong") is not None


if __name__ == "__main__":
    sys.exit(main())
