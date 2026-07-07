"""Generate an exact spherical quadrature grid for the WP3 orthonormality test.

requires: python >= 3.12, mpmath == 1.3.0

Regenerate:
    .venv\\Scripts\\python tools\\reference\\gen_sh_quadrature.py

Output: tests/data/sh_quadrature.json

Grid: Gauss-Legendre(11) nodes in x = cos(colatitude) x 24 uniform azimuths =
264 points. Exactness argument (this is why no external t-design is needed):
a product Y_a * Y_b for orders a,b <= 10 has azimuthal frequency content
k <= 20 < 24, so the uniform-azimuth rule integrates it exactly; after azimuth
integration only m_a == m_b terms survive, whose x-dependence is a polynomial
of degree a+b <= 20 <= 2*11-1, integrated exactly by GL-11. So the discrete
sum sum_i w_i Y_a(d_i) Y_b(d_i) is mathematically exact for the whole order-10
band. A genuine t-design (arriving in WP7 for AllRAD) would need a comparable
point count plus an external data dependency.

Weights are normalized so sum(w) = 1 (mean over the sphere), which makes the
N3D orthonormality read sum_i w_i Y^N3D_a Y^N3D_b = delta_ab with no 4pi factor.
"""

import datetime
import json
import math
import os

import mpmath as mp

import shref_common as sh  # imports trigger the pinned-version gate + self-check

GL_NODES = 11
N_AZIMUTH = 24
DPS = 50


def main():
    mp.mp.dps = DPS
    x_nodes, x_weights = sh.gauss_legendre_nodes_weights(GL_NODES)

    points = []
    total_weight = mp.mpf(0)
    for x, wx in zip(x_nodes, x_weights):
        # x = cos(colatitude) = sin(elevation)
        el = mp.asin(mp.mpf(x))
        for j in range(N_AZIMUTH):
            az = -mp.pi + 2 * mp.pi * j / N_AZIMUTH
            # azimuth weight 2pi/N over [-pi,pi]; sphere area normalization /(4pi)
            w = wx * (2 * mp.pi / N_AZIMUTH) / (4 * mp.pi)
            total_weight += w
            points.append({
                "azimuthDeg": float(mp.degrees(az)),
                "elevationDeg": float(mp.degrees(el)),
                "weight": float(w),
            })

    # self-check: weights sum to 1 (mean over the sphere)
    if abs(total_weight - 1) > mp.mpf(10) ** -30:
        raise SystemExit(f"quadrature self-check FAILED: sum(w) = {total_weight}, expected 1")

    # self-check: a few N3D orthonormality integrals equal delta to 30 digits
    def y_n3d(l, m, az_r, el_r):
        return sh.real_sh_sn3d(l, m, az_r, el_r) * mp.sqrt(2 * l + 1)

    checks = [((0, 0), (0, 0), 1), ((1, 1), (1, 1), 1), ((2, -1), (2, -1), 1),
              ((1, 1), (2, 0), 0), ((3, 2), (1, -1), 0)]
    for (la, ma), (lb, mb), expected in checks:
        acc = mp.mpf(0)
        for x, wx in zip(x_nodes, x_weights):
            el = mp.asin(mp.mpf(x))
            for j in range(N_AZIMUTH):
                az = -mp.pi + 2 * mp.pi * j / N_AZIMUTH
                w = wx * (2 * mp.pi / N_AZIMUTH) / (4 * mp.pi)
                acc += w * y_n3d(la, ma, az, el) * y_n3d(lb, mb, az, el)
        if abs(acc - expected) > mp.mpf(10) ** -30:
            raise SystemExit(
                f"quadrature orthonormality self-check FAILED for "
                f"({la},{ma})x({lb},{mb}): got {acc}, expected {expected}")

    doc = {
        "provenance": {
            "script": "tools/reference/gen_sh_quadrature.py",
            "command": ".venv/Scripts/python tools/reference/gen_sh_quadrature.py",
            "python": sh.PYTHON_VERSION,
            "mpmath": sh.MPMATH_VERSION,
            "generatedUtc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "grid": f"Gauss-Legendre({GL_NODES}) in cos-colatitude x {N_AZIMUTH} uniform azimuths",
            "exactness": "exact for products of SH up to order 10 (band 20 < azimuth "
                         "count 24; GL-11 exact to polynomial degree 21)",
            "weightNormalization": "sum(w) = 1 (mean over the sphere)",
            "testTolerance": 1e-10,
        },
        "numPoints": len(points),
        "points": points,
    }

    out_path = os.path.abspath(os.path.join(
        os.path.dirname(__file__), "..", "..", "tests", "data", "sh_quadrature.json"))
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")
    print(f"wrote {out_path} ({len(points)} points, sum(w)=1)")


if __name__ == "__main__":
    main()
