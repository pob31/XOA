"""Generate golden real-SH values for the WP3 tests.

requires: python >= 3.12, mpmath == 1.3.0

Regenerate:
    .venv\\Scripts\\python tools\\reference\\gen_sh_reference.py

Output: tests/data/sh_reference.json  (order 10; lower orders are prefixes).
Convention and the complex->real conversion live in shref_common.py.
"""

import datetime
import json
import math
import os
import random

import mpmath as mp

import shref_common as sh

ORDER = 10


def directions():
    """Cardinals, pole/near-pole/wrap probes, a diagonal, and seeded randoms.

    Values are (azimuthDeg, elevationDeg). Elevation +90/-90 are the poles;
    m != 0 channels must vanish there. Near-pole (+/-89.9) stresses the
    recurrence. az 180 / -179.9 probe azimuth wrap.
    """
    fixed = [
        (0.0, 0.0),      # +X front
        (90.0, 0.0),     # +Y left
        (180.0, 0.0),    # -X back
        (-90.0, 0.0),    # -Y right
        (0.0, 90.0),     # +Z up (pole)
        (0.0, -90.0),    # -Z down (pole)
        (123.4, 90.0),   # pole with nonzero az (m!=0 must still vanish)
        (123.4, -90.0),
        (45.0, 89.9),    # near-pole
        (200.0, -89.9),
        (180.0, 10.0),   # az wrap boundary
        (-179.9, -5.0),
        (45.0, 35.26438968275466),   # cube body diagonal (atan(1/sqrt2))
    ]
    rng = random.Random(20260707)  # documented seed: 2026-07-07
    randoms = []
    for _ in range(20):
        # uniform on the sphere: z = 2u-1, azimuth uniform
        z = 2.0 * rng.random() - 1.0
        az = 360.0 * rng.random() - 180.0
        el = math.degrees(math.asin(z))
        randoms.append((az, el))
    return fixed + randoms


def main():
    dirs = directions()
    entries = []
    for az_deg, el_deg in dirs:
        az = math.radians(az_deg)
        el = math.radians(el_deg)
        entries.append({
            "azimuthDeg": az_deg,
            "elevationDeg": el_deg,
            "sh": sh.real_sh_vector(ORDER, az, el),
        })

    doc = {
        "provenance": {
            "script": "tools/reference/gen_sh_reference.py",
            "command": ".venv/Scripts/python tools/reference/gen_sh_reference.py",
            "python": sh.PYTHON_VERSION,
            "mpmath": sh.MPMATH_VERSION,
            "generatedUtc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "convention": "ACN ordering, SN3D normalization, no Condon-Shortley (AmbiX); "
                          "az degrees CCW from +X; el degrees from horizon",
            "references": ["Nachbar, Zotter, Deleflie, Sontacchi 2011 (AmbiX format)"],
            "testTolerance": 1e-13,
        },
        "order": ORDER,
        "directions": entries,
    }

    out_path = os.path.join(os.path.dirname(__file__), "..", "..", "tests", "data", "sh_reference.json")
    out_path = os.path.abspath(out_path)
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")
    print(f"wrote {out_path} ({len(entries)} directions, order {ORDER})")


if __name__ == "__main__":
    main()
