"""Generate the AllRAD virtual-layout t-design tables (WP7).

requires: python >= 3.12, mpmath == 1.3.0

Source: Womersley symmetric (antipodal) spherical t-design, strength t = 33,
N = 564 points, downloaded from
  https://web.maths.unsw.edu.au/~rsw/Sphere/Points/SS/SS31-Mar-2016/ss033.00564
and cached verbatim as tools/reference/ss033.00564 (committed, so regeneration
is offline-reproducible). Chosen per XOA-DEVPLAN WP7: strength must be
>= 21 (= 2N+1 for the order-10 bus) with a point count in the ~500-1000
"dense" band; the actual t=31 file has 498 points (just under the band), so
the next symmetric design t=33 / 564 points is used. Reference:
  R. S. Womersley, "Efficient Spherical Designs with Good Geometric
  Properties" (2018); point sets from
  https://web.maths.unsw.edu.au/~rsw/Sphere/EffSphDes/

A strength-t design integrates every spherical harmonic of degree <= t
exactly with UNIFORM weights: (4pi/N) * sum_i Y(x_i) == integral(Y). That is
what makes the AllRAD virtual sampling decode exact at order 10.

Self-checks (the quality gate -- correctness never depends on trusting the
download):
  1. count == 564; every point unit-norm to < 1e-14;
  2. antipodal symmetry: every point's negation is in the set (< 1e-13),
     which makes all ODD-degree SH sums vanish by construction;
  3. quadrature: for every ACN channel c with 1 <= degree <= 33,
     |sum_i Y_c(x_i)| < 1e-9 (SN3D, via shref_common.real_sh_sn3d at 40 dps),
     and the degree-0 sum == N exactly (SN3D Y_0 == 1).
The committed C++ table is INDEPENDENTLY re-verified up to degree 21 by
xoa-tests with the production evaluator (tests/XoaDecoderTests.cpp), so the
artifact is checked by two implementations.

Outputs (both committed):
  Source/DSP/TDesignTables.h        -- xoa::tdesign::{kStrength, kCount, kPoints}
  tools/reference/tdesign_data.json -- same points + provenance, consumed by
                                       gen_decoder_reference.py so the Python
                                       AllRAD goldens use the bit-identical
                                       virtual layout.
"""

import json
import math
import pathlib
import sys

import mpmath as mp

import shref_common as sh

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent

SOURCE_FILE = HERE / "ss033.00564"
SOURCE_URL = ("https://web.maths.unsw.edu.au/~rsw/Sphere/Points/SS/"
              "SS31-Mar-2016/ss033.00564")
STRENGTH = 33
COUNT = 564

HEADER_OUT = REPO / "Source" / "DSP" / "TDesignTables.h"
JSON_OUT = HERE / "tdesign_data.json"

QUADRATURE_TOL = 1.0e-9
UNIT_NORM_TOL = 1.0e-14
ANTIPODAL_TOL = 1.0e-13


def load_points():
    if not SOURCE_FILE.exists():
        sys.exit(f"gen_tdesign_tables: cached source {SOURCE_FILE.name} missing.\n"
                 f"Download it first:  curl -o {SOURCE_FILE} {SOURCE_URL}")
    pts = []
    for line in SOURCE_FILE.read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        x, y, z = (float(tok) for tok in line.split())
        pts.append((x, y, z))
    return pts


def check_structure(pts):
    if len(pts) != COUNT:
        sys.exit(f"FAILED: expected {COUNT} points, got {len(pts)}")

    for i, (x, y, z) in enumerate(pts):
        err = abs(math.sqrt(x * x + y * y + z * z) - 1.0)
        if err > UNIT_NORM_TOL:
            sys.exit(f"FAILED: point {i} norm error {err:.3e} > {UNIT_NORM_TOL}")

    # Antipodal pairing: every -p must be present (proves odd degrees vanish).
    def key(p):
        return (round(p[0], 10), round(p[1], 10), round(p[2], 10))

    index = {key(p): i for i, p in enumerate(pts)}
    for i, (x, y, z) in enumerate(pts):
        neg = (-x, -y, -z)
        j = index.get(key(neg))
        ok = False
        if j is not None:
            q = pts[j]
            ok = max(abs(neg[0] - q[0]), abs(neg[1] - q[1]), abs(neg[2] - q[2])) < ANTIPODAL_TOL
        if not ok:
            sys.exit(f"FAILED: point {i} has no antipodal partner within {ANTIPODAL_TOL}")
    print(f"structure OK: {COUNT} unit points, antipodally symmetric")


def check_quadrature(pts):
    """|sum_i Y_c(x_i)| < tol for every channel with 1 <= degree <= STRENGTH."""
    nch = (STRENGTH + 1) * (STRENGTH + 1)
    sums = [mp.mpf(0)] * nch
    for i, (x, y, z) in enumerate(pts):
        az = math.atan2(y, x)
        el = math.asin(max(-1.0, min(1.0, z)))
        for l in range(STRENGTH + 1):
            for m in range(-l, l + 1):
                sums[sh.acn(l, m)] += sh.real_sh_sn3d(l, m, az, el)
        if (i + 1) % 50 == 0:
            print(f"  quadrature: {i + 1}/{len(pts)} points...")

    # Degree 0: SN3D Y_0 == 1 -> the sum is exactly N.
    if abs(sums[0] - COUNT) > 1.0e-9:
        sys.exit(f"FAILED: degree-0 sum {sums[0]} != {COUNT}")

    worst = mp.mpf(0)
    worst_c = -1
    for c in range(1, nch):
        a = abs(sums[c])
        if a > worst:
            worst, worst_c = a, c
    print(f"quadrature OK: worst |sum Y_c| = {mp.nstr(worst, 3)} (acn {worst_c}), "
          f"tolerance {QUADRATURE_TOL}")
    if worst > QUADRATURE_TOL:
        sys.exit(f"FAILED: quadrature residual {mp.nstr(worst, 6)} at acn {worst_c} "
                 f"exceeds {QUADRATURE_TOL} -- not a strength-{STRENGTH} design")


def g17(v):
    return f"{v:.17g}"


def write_header(pts):
    lines = []
    lines.append("#pragma once")
    lines.append("")
    lines.append("//==============================================================================")
    lines.append("// XOA - AllRAD virtual-layout spherical t-design (WP7). GENERATED FILE -")
    lines.append("// regenerate with tools/reference/gen_tdesign_tables.py; do not edit by hand.")
    lines.append("//")
    lines.append(f"// Womersley symmetric spherical design, strength t = {STRENGTH}, N = {COUNT}")
    lines.append("// unit vectors. A strength-t design integrates every spherical harmonic of")
    lines.append("// degree <= t exactly with uniform weights, which makes the AllRAD virtual")
    lines.append("// sampling decode exact at order 10 (needs t >= 21). Source and self-checks")
    lines.append("// (unit norms, antipodal symmetry, SH quadrature at 40-digit precision) are")
    lines.append("// documented in the generator; xoa-tests independently re-verifies this")
    lines.append("// committed table up to degree 21 with the production SH evaluator.")
    lines.append("//")
    lines.append("// Reference: R. S. Womersley, \"Efficient Spherical Designs with Good")
    lines.append("// Geometric Properties\" (2018),")
    lines.append("// https://web.maths.unsw.edu.au/~rsw/Sphere/EffSphDes/")
    lines.append("//==============================================================================")
    lines.append("")
    lines.append("namespace xoa::tdesign")
    lines.append("{")
    lines.append("")
    lines.append(f"constexpr int kStrength = {STRENGTH};")
    lines.append(f"constexpr int kCount = {COUNT};")
    lines.append("")
    lines.append("constexpr double kPoints[kCount][3] = {")
    for (x, y, z) in pts:
        lines.append(f"    {{ {g17(x)}, {g17(y)}, {g17(z)} }},")
    lines.append("};")
    lines.append("")
    lines.append("} // namespace xoa::tdesign")
    lines.append("")
    HEADER_OUT.write_text("\n".join(lines), newline="\n")
    print(f"wrote {HEADER_OUT}")


def write_json(pts):
    doc = {
        "provenance": {
            "generator": "tools/reference/gen_tdesign_tables.py",
            "python": sh.PYTHON_VERSION,
            "mpmath": sh.MPMATH_VERSION,
            "source": SOURCE_URL,
            "cachedCopy": f"tools/reference/{SOURCE_FILE.name}",
            "reference": ("R. S. Womersley, 'Efficient Spherical Designs with Good "
                          "Geometric Properties' (2018)"),
            "strength": STRENGTH,
            "count": COUNT,
            "quadratureTolerance": QUADRATURE_TOL,
            "notes": ("Symmetric (antipodal) design; uniform-weight SH quadrature exact "
                      "to degree <= strength. Consumed by gen_decoder_reference.py so "
                      "Python AllRAD goldens share the C++ virtual layout bit-exactly."),
        },
        "strength": STRENGTH,
        "count": COUNT,
        "points": [[float(g17(x)), float(g17(y)), float(g17(z))] for (x, y, z) in pts],
    }
    JSON_OUT.write_text(json.dumps(doc, indent=1), newline="\n")
    print(f"wrote {JSON_OUT}")


def main():
    pts = load_points()
    check_structure(pts)
    check_quadrature(pts)
    write_header(pts)
    write_json(pts)
    print("gen_tdesign_tables: all checks passed")


if __name__ == "__main__":
    main()
