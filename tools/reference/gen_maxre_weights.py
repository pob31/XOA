"""Generate golden max-rE and in-phase order weights for the WP3 tests.

requires: python >= 3.12, mpmath == 1.3.0

Regenerate:
    .venv\\Scripts\\python tools\\reference\\gen_maxre_weights.py

Output: tests/data/order_weights.json  (orders 1..10).

max-rE: g_l = P_l(r_E), r_E = largest root of P_{N+1}.
in-phase: g_l = N!(N+1)!/((N+l+1)!(N-l)!), computed with exact Fractions.

Self-checks (abort on mismatch):
  * exact closed-form r_E for N=1..3 (1/sqrt3, sqrt(3/5), sqrt((3+2 sqrt(6/5))/7))
  * Abramowitz & Stegun Table 25.4 Gauss-Legendre abscissas (largest node) N=1..9
  * seed-formula band |r_E - cos(137.9deg/(N+1.51))| < 5e-3 for all N
"""

import datetime
import json
import os
from fractions import Fraction

import mpmath as mp

import shref_common as sh  # noqa: F401  (pinned-version gate + self-check)

MAX_ORDER = 10
DPS = 50


def max_re_cosine(order):
    """Largest root of P_{order+1} = largest Gauss-Legendre node (Golub-Welsch,
    high precision, no seed-convergence risk)."""
    mp.mp.dps = DPS
    nodes, _ = sh.gauss_legendre_nodes_weights(order + 1)
    return nodes[-1]   # ascending -> last is largest


def in_phase_weights(order):
    """Exact in-phase weights as Fractions via the ratio recurrence."""
    g = [Fraction(1)]
    for l in range(1, order + 1):
        g.append(g[-1] * Fraction(order - l + 1, order + l + 1))
    return g


# Abramowitz & Stegun, Handbook of Mathematical Functions, Table 25.4:
# largest Gauss-Legendre abscissa x_n for n = 2..10 (n = order+1).
AS_LARGEST_ABSCISSA = {
    2: mp.mpf("0.577350269189625764509148780502"),   # 1/sqrt3
    3: mp.mpf("0.774596669241483377035853079956"),
    4: mp.mpf("0.861136311594052575223946488893"),
    5: mp.mpf("0.906179845938663992797626878299"),
    6: mp.mpf("0.932469514203152027812301554494"),
    7: mp.mpf("0.949107912342758524526189684048"),
    8: mp.mpf("0.960289856497536231683560868569"),
    9: mp.mpf("0.968160239507626089835576203"),
    10: mp.mpf("0.973906528517171720077964012"),
    11: mp.mpf("0.978228658146056992803938001"),
}


def main():
    mp.mp.dps = DPS
    orders_out = []

    for N in range(1, MAX_ORDER + 1):
        rE = max_re_cosine(N)

        # self-check vs Abramowitz & Stegun largest abscissa of P_{N+1}
        as_ref = AS_LARGEST_ABSCISSA[N + 1]
        if abs(rE - as_ref) > mp.mpf(10) ** -12:
            raise SystemExit(f"max-rE self-check FAILED N={N}: r_E={rE}, A&S={as_ref}")

        # self-check vs exact closed forms for N=1..3
        exact = {
            1: 1 / mp.sqrt(3),
            2: mp.sqrt(mp.mpf(3) / 5),
            3: mp.sqrt((3 + 2 * mp.sqrt(mp.mpf(6) / 5)) / 7),
        }
        if N in exact and abs(rE - exact[N]) > mp.mpf(10) ** -30:
            raise SystemExit(f"max-rE closed-form self-check FAILED N={N}")

        # seed-band sanity (Zotter-Frank approximation)
        seed = mp.cos(mp.radians(mp.mpf("137.9") / (N + mp.mpf("1.51"))))
        if abs(rE - seed) > mp.mpf("5e-3"):
            raise SystemExit(f"max-rE seed-band self-check FAILED N={N}: |{rE}-{seed}|")

        max_re = [float(mp.legendre(l, rE)) for l in range(N + 1)]
        in_phase = [float(g) for g in in_phase_weights(N)]

        orders_out.append({
            "order": N,
            "rE": float(rE),
            "maxRe": max_re,
            "inPhase": in_phase,
        })

    doc = {
        "provenance": {
            "script": "tools/reference/gen_maxre_weights.py",
            "command": ".venv/Scripts/python tools/reference/gen_maxre_weights.py",
            "python": sh.PYTHON_VERSION,
            "mpmath": sh.MPMATH_VERSION,
            "generatedUtc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "definitions": "maxRe[l]=P_l(r_E), r_E=largest root of P_{N+1}; "
                           "inPhase[l]=N!(N+1)!/((N+l+1)!(N-l)!)",
            "references": ["Zotter & Frank, Ambisonics (Springer 2019) ch.4",
                           "Abramowitz & Stegun, Table 25.4 (GL abscissas)"],
            "testTolerance": 1e-14,
        },
        "orders": orders_out,
    }

    out_path = os.path.abspath(os.path.join(
        os.path.dirname(__file__), "..", "..", "tests", "data", "order_weights.json"))
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")
    print(f"wrote {out_path} (orders 1..{MAX_ORDER})")


if __name__ == "__main__":
    main()
