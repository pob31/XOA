"""Generate the FuMa->AmbiX conversion table and test vectors for WP3.

requires: python >= 3.12, mpmath == 1.3.0

Regenerate:
    .venv\\Scripts\\python tools\\reference\\gen_fuma_reference.py

Output: tests/data/fuma_reference.json

The FuMa->SN3D gains are derived HERE from first principles (the maxN rule),
NOT transcribed from the C++ table, so the S11 test is not self-referential:

  FuMa is maxN-normalized: every FuMa basis function peaks at magnitude 1 over
  the sphere (W additionally carries the 1/sqrt2 pressure factor). Hence
    fumaToSn3dGain(l,m) = max_sphere |Y^SN3D_{l,m}|          (l,m) != (0,0)
    fumaToSn3dGain(0,0) = sqrt2 * max_sphere |Y^SN3D_{0,0}| = sqrt2.
  The peak magnitude depends only on the Schmidt Legendre factor (the azimuth
  factor peaks at 1), so it is a 1-D maximization of |Y^SN3D_{l,|m|}(az=0, el)|
  over elevation.

Each computed gain is asserted equal to its published sqrt-rational closed form
to 30 digits (this IS the numeric cite-check). A test vector is then built as
fuma[f] = ambix[acn(f)] / gain(f), with ambix computed independently by mpmath.
"""

import datetime
import json
import math
import os

import mpmath as mp

import shref_common as sh

DPS = 40

# FuMa channel order WXYZ RSTUV KLMNOPQ, with (name, l, m) and the published
# closed-form gain (FuMa -> SN3D). The generator verifies each gain numerically.
FUMA_ORDER = [
    ("W", 0, 0, lambda: mp.sqrt(2)),
    ("X", 1, 1, lambda: mp.mpf(1)),
    ("Y", 1, -1, lambda: mp.mpf(1)),
    ("Z", 1, 0, lambda: mp.mpf(1)),
    ("R", 2, 0, lambda: mp.mpf(1)),
    ("S", 2, 1, lambda: mp.sqrt(3) / 2),
    ("T", 2, -1, lambda: mp.sqrt(3) / 2),
    ("U", 2, 2, lambda: mp.sqrt(3) / 2),
    ("V", 2, -2, lambda: mp.sqrt(3) / 2),
    ("K", 3, 0, lambda: mp.mpf(1)),
    ("L", 3, 1, lambda: mp.sqrt(mp.mpf(32) / 45)),
    ("M", 3, -1, lambda: mp.sqrt(mp.mpf(32) / 45)),
    ("N", 3, 2, lambda: mp.sqrt(5) / 3),
    ("O", 3, -2, lambda: mp.sqrt(5) / 3),
    ("P", 3, 3, lambda: mp.sqrt(mp.mpf(5) / 8)),
    ("Q", 3, -3, lambda: mp.sqrt(mp.mpf(5) / 8)),
]


def peak_magnitude(l, m):
    """max over elevation of |Y^SN3D_{l,|m|}(az=0, el)| = max |P_bar_l^|m|(sin el)|."""
    mp.mp.dps = DPS
    am = abs(m)

    def mag(el):
        return abs(sh.real_sh_sn3d(l, am, mp.mpf(0), el))

    # coarse scan for the argmax, then polish on the derivative
    best_el, best_val = mp.mpf(0), mp.mpf(-1)
    steps = 2000
    for i in range(steps + 1):
        el = -mp.pi / 2 + mp.pi * i / steps
        v = mag(el)
        if v > best_val:
            best_val, best_el = v, el
    # polish: root of d/del |Y| via derivative of the signed value near the peak
    try:
        peak_el = mp.findroot(lambda e: mp.diff(lambda t: sh.real_sh_sn3d(l, am, mp.mpf(0), t), e),
                              best_el)
        return abs(sh.real_sh_sn3d(l, am, mp.mpf(0), peak_el))
    except Exception:
        return best_val


def acn(l, m):
    return l * (l + 1) + m


def directions():
    fixed = [(0.0, 0.0), (90.0, 0.0), (180.0, 0.0), (-90.0, 0.0),
             (0.0, 90.0), (0.0, -90.0)]
    import random
    rng = random.Random(31415926)
    rnd = []
    for _ in range(8):
        z = 2.0 * rng.random() - 1.0
        az = 360.0 * rng.random() - 180.0
        el = math.degrees(math.asin(z))
        rnd.append((az, el))
    return fixed + rnd


def main():
    mp.mp.dps = DPS

    # Build and verify the table.
    table = []
    gains = []
    for f, (name, l, m, closed) in enumerate(FUMA_ORDER):
        peak = peak_magnitude(l, m)
        gain_expected = closed()
        gain = mp.sqrt(2) if (l == 0 and m == 0) else peak
        # numeric cite-check: derived gain == published closed form
        if abs(gain - gain_expected) > mp.mpf(10) ** -30:
            raise SystemExit(
                f"FuMa gain self-check FAILED {name} (l={l},m={m}): "
                f"maxN-derived {gain} != closed form {gain_expected}")
        gains.append(gain)
        table.append({"fumaIndex": f, "name": name, "l": l, "m": m,
                      "acn": acn(l, m), "gain": float(gain)})

    # Test vectors: fuma[f] = ambix[acn(f)] / gain(f); ambix from mpmath.
    dir_entries = []
    for az_deg, el_deg in directions():
        az = mp.radians(mp.mpf(az_deg))
        el = mp.radians(mp.mpf(el_deg))
        ambix = [float(sh.real_sh_sn3d(l, m, az, el))
                 for l in range(4) for m in range(-l, l + 1)]
        fuma = []
        for f, (name, l, m, _closed) in enumerate(FUMA_ORDER):
            a = sh.real_sh_sn3d(l, m, az, el)
            fuma.append(float(a / gains[f]))
        dir_entries.append({
            "azimuthDeg": az_deg,
            "elevationDeg": el_deg,
            "fuma": fuma,     # WXYZ RSTUV KLMNOPQ order
            "ambix": ambix,   # ACN/SN3D order, 16 channels
        })

    doc = {
        "provenance": {
            "script": "tools/reference/gen_fuma_reference.py",
            "command": ".venv/Scripts/python tools/reference/gen_fuma_reference.py",
            "python": sh.PYTHON_VERSION,
            "mpmath": sh.MPMATH_VERSION,
            "generatedUtc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "rule": "FuMa is maxN; gain(l,m)=max_sphere|Y^SN3D_{l,m}| (W: x sqrt2). "
                    "Gains derived here and asserted == published sqrt-rationals.",
            "references": ["Malham FMH definitions", "Daniel 2000 thesis section 3.3",
                           "Kronlachner 2014 ambix_converter"],
            "testTolerance": 1e-13,
        },
        "fumaChannelOrder": "WXYZ RSTUV KLMNOPQ",
        "table": table,
        "directions": dir_entries,
    }

    out_path = os.path.abspath(os.path.join(
        os.path.dirname(__file__), "..", "..", "tests", "data", "fuma_reference.json"))
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")
    print(f"wrote {out_path} ({len(dir_entries)} directions; table verified)")


if __name__ == "__main__":
    main()
