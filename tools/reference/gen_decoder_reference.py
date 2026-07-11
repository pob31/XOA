"""Generate golden decoder matrices, SVD data, and rV/rE for the WP5 tests.

requires: python >= 3.12, mpmath == 1.3.0

Regenerate:
    .venv\\Scripts\\python tools\\reference\\gen_decoder_reference.py

Output: tests/data/decoder_reference.json

Deviation from DEVPLAN wording ("octave/IEM reference"): uses mpmath (mp.svd_r,
~40-digit) like the WP3/WP4 generators. The complex->real SH conversion lives in
shref_common.py; the decoder formulas below are pinned to the WP5 plan and are
implemented identically in Source/DSP/AmbiDecoderDesigner.h.

Formulas (SN3D bus in, speaker gains out; D is L x K, row-major [speaker][acn]):
  SAD:   D0[s][c] = (1/L)*(2 l_c + 1)*Y^SN3D_c(u_s);   weighted: * g_{l_c}
  mode:  D = pinv(Y^N3D) @ diag(g_{l_c}) @ diag(sqrt(2 l_c+1)),
         Y^N3D[c][s] = sqrt(2 l_c+1)*Y^SN3D_c(u_s)
  norm (global scalar): amplitude a_A = 1/sum_s D[s][0];
                        energy    a_E = 1/sqrt(sum_s sum_c D[s][c]^2/(2 l_c+1))
"""

import datetime
import json
import math
import os

import mpmath as mp

import shref_common as sh

DPS = 40
RANK_TOL_REL = mp.mpf(10) ** -9


def acn(l, m):
    return l * (l + 1) + m


def channel_orders(order):
    """order l of each ACN channel 0..(order+1)^2-1."""
    orders = []
    for l in range(order + 1):
        for m in range(-l, l + 1):
            orders.append(l)
    return orders


def max_design_order(num_speakers):
    return min(10, int(math.isqrt(num_speakers)) - 1)


def rE_cosine(order):
    """max-rE characteristic value: largest Gauss-Legendre node of P_{order+1}."""
    nodes, _ = sh.gauss_legendre_nodes_weights(order + 1)
    return nodes[-1]


def order_weights(order, weighting):
    if weighting == "basic":
        return [mp.mpf(1)] * (order + 1)
    rE = rE_cosine(order)
    return [mp.legendre(l, rE) for l in range(order + 1)]


def unit_directions(positions):
    """Cartesian positions -> (az_rad, el_rad, unit xyz)."""
    dirs = []
    for x, y, z in positions:
        r = mp.sqrt(x * x + y * y + z * z)
        if r == 0:
            dirs.append((mp.mpf(0), mp.mpf(0), (mp.mpf(1), mp.mpf(0), mp.mpf(0))))
        else:
            az = mp.atan2(y, x)
            el = mp.asin(z / r)
            dirs.append((az, el, (x / r, y / r, z / r)))
    return dirs


def sh_matrix_sn3d(positions, order):
    """Y^SN3D[c][s], K x L."""
    dirs = unit_directions(positions)
    K = (order + 1) * (order + 1)
    L = len(positions)
    Y = mp.zeros(K, L)
    for s, (az, el, _u) in enumerate(dirs):
        for l in range(order + 1):
            for m in range(-l, l + 1):
                Y[acn(l, m), s] = sh.real_sh_sn3d(l, m, az, el)
    return Y


def normalize_scalar(D, orders, mode):
    """Global normalization scalar for the weighted matrix D (L x K)."""
    L = D.rows
    if mode == "amplitude":
        w_sum = mp.fsum(D[s, 0] for s in range(L))
        return 1 / w_sum if w_sum != 0 else mp.mpf(1)
    # energy
    acc = mp.mpf(0)
    for s in range(L):
        for c in range(D.cols):
            acc += D[s, c] * D[s, c] / (2 * orders[c] + 1)
    return 1 / mp.sqrt(acc) if acc != 0 else mp.mpf(1)


def sad_matrix(positions, order, weighting, norm_mode):
    Y = sh_matrix_sn3d(positions, order)
    orders = channel_orders(order)
    g = order_weights(order, weighting)
    L = len(positions)
    K = Y.rows
    D = mp.zeros(L, K)
    for s in range(L):
        for c in range(K):
            D[s, c] = (mp.mpf(1) / L) * (2 * orders[c] + 1) * Y[c, s] * g[orders[c]]
    D *= normalize_scalar(D, orders, norm_mode)
    return D


def truncated_pinv(A, lam_rel=mp.mpf(0)):
    """Pseudo-inverse of A (K x L) via truncated SVD + optional Tikhonov.
    Returns (pinv L x K, singular_values desc, sigmaMax, sigmaMin, rank)."""
    U, S, V = mp.svd_r(A)   # A = U diag(S) V ; U KxK, V KxL (K<=L)
    K = A.rows
    L = A.cols
    # convention guard: reconstruction must match
    recon = U * mp.diag(S) * V
    err = max(abs(recon[i, j] - A[i, j]) for i in range(K) for j in range(L))
    if err > mp.mpf(10) ** -30:
        raise SystemExit(f"svd_r reconstruction self-check FAILED: {err}")

    sigmas = [S[i] for i in range(len(S))]
    sigma_max = sigmas[0]
    sigma_min = sigmas[-1]
    cutoff = RANK_TOL_REL * sigma_max
    lam = lam_rel * sigma_max
    rank = 0
    f = []
    for sv in sigmas:
        if sv > cutoff:
            f.append(sv / (sv * sv + lam * lam))
            rank += 1
        else:
            f.append(mp.mpf(0))
    # pinv = V^T diag(f) U^T  (L x K)
    pinv = V.T * mp.diag(f) * U.T
    return pinv, sigmas, sigma_max, sigma_min, rank


def mode_match_matrix(positions, order, weighting, norm_mode, lam_rel=mp.mpf(0)):
    Ysn = sh_matrix_sn3d(positions, order)
    orders = channel_orders(order)
    K = Ysn.rows
    L = Ysn.cols
    # Y^N3D = diag(sqrt(2l+1)) Ysn
    Yn3d = mp.zeros(K, L)
    for c in range(K):
        Yn3d[c, :] = mp.sqrt(2 * orders[c] + 1) * Ysn[c, :]
    pinv, sigmas, smax, smin, rank = truncated_pinv(Yn3d, lam_rel)   # L x K
    g = order_weights(order, weighting)
    D = mp.zeros(L, K)
    for s in range(L):
        for c in range(K):
            D[s, c] = pinv[s, c] * g[orders[c]] * mp.sqrt(2 * orders[c] + 1)
    D *= normalize_scalar(D, orders, norm_mode)
    return D, sigmas, smax, smin, rank


def condition_number(sigmas):
    smax, smin = sigmas[0], sigmas[-1]
    return (smax / smin) if smin > 0 else mp.inf


def rv_re(D, positions, az_deg, el_deg, order):
    """rV/rE magnitudes + direction errors (deg) for a source direction."""
    az = mp.radians(mp.mpf(az_deg))
    el = mp.radians(mp.mpf(el_deg))
    a = mp.zeros((order + 1) * (order + 1), 1)
    for l in range(order + 1):
        for m in range(-l, l + 1):
            a[acn(l, m), 0] = sh.real_sh_sn3d(l, m, az, el)
    dirs = unit_directions(positions)
    L = len(positions)
    g = [mp.fsum(D[s, c] * a[c, 0] for c in range(D.cols)) for s in range(L)]

    P = mp.fsum(g)
    E = mp.fsum(gs * gs for gs in g)
    d = (mp.cos(el) * mp.cos(az), mp.cos(el) * mp.sin(az), mp.sin(el))

    def vec(weights, denom):
        if denom == 0:
            return None
        return tuple(mp.fsum(weights[s] * dirs[s][2][k] for s in range(L)) / denom for k in range(3))

    rv = vec(g, P)
    re = vec([gs * gs for gs in g], E)

    def mag_err(v):
        if v is None:
            return None, None
        mag = mp.sqrt(sum(vi * vi for vi in v))
        if mag == 0:
            return mp.mpf(0), None
        cosang = sum(v[k] * d[k] for k in range(3)) / mag
        cosang = max(mp.mpf(-1), min(mp.mpf(1), cosang))
        return mag, mp.degrees(mp.acos(cosang))

    rv_mag, rv_err = mag_err(rv)
    re_mag, re_err = mag_err(re)
    return {
        "azimuthDeg": float(az_deg), "elevationDeg": float(el_deg),
        "rvMagnitude": float(rv_mag) if rv_mag is not None else None,
        "rvDirErrorDeg": float(rv_err) if rv_err is not None else None,
        "reMagnitude": float(re_mag) if re_mag is not None else None,
        "reDirErrorDeg": float(re_err) if re_err is not None else None,
        "energy": float(E),
    }


# -- fixtures ------------------------------------------------------------------
def ring24():
    r = mp.mpf(2)
    pos = []
    for k in range(24):
        az = mp.radians(mp.mpf(k) * 15)
        pos.append((r * mp.cos(az), r * mp.sin(az), mp.mpf(0)))
    return pos


def dome24():
    # 24-speaker hemispherical dome, r=3: auto design order floor(sqrt(24))-1=3.
    # Four staggered elevation rings for conditioning (a strict hemisphere is
    # inherently more ill-conditioned than a sphere -- that is why classify()
    # suggests SAD for domes; AllRAD+imaginary floor speaker is WP7).
    r = mp.mpf(3)
    rings = [(mp.mpf(0), 12, mp.mpf(0)),
             (mp.mpf(30), 7, mp.mpf("12.857")),
             (mp.mpf(55), 4, mp.mpf(45)),
             (mp.mpf(80), 1, mp.mpf(0))]
    pos = []
    for el_deg, count, az0 in rings:
        el = mp.radians(el_deg)
        for k in range(count):
            az = mp.radians(az0 + mp.mpf(k) * 360 / count)
            pos.append((r * mp.cos(el) * mp.cos(az), r * mp.cos(el) * mp.sin(az), r * mp.sin(el)))
    return pos


def icosahedron12():
    phi = (1 + mp.sqrt(5)) / 2
    raw = []
    for a in (mp.mpf(1), mp.mpf(-1)):
        for b in (phi, -phi):
            raw.append((mp.mpf(0), a, b))
            raw.append((a, b, mp.mpf(0)))
            raw.append((b, mp.mpf(0), a))
    norm = mp.sqrt(1 + phi * phi)
    r = mp.mpf(2)
    return [(r * x / norm, r * y / norm, r * z / norm) for (x, y, z) in raw]


COMBOS = [(t, w, n) for t in ("sad", "modeMatch")
          for w in ("basic", "maxRe") for n in ("amplitude", "energy")]


def matrix_to_list(D):
    return [[float(D[s, c]) for c in range(D.cols)] for s in range(D.rows)]


def build_fixture(name, positions):
    mp.mp.dps = DPS
    L = len(positions)
    order = max_design_order(L)
    orders = channel_orders(order)

    matrices = {}
    sigmas_ref = None
    for (t, w, n) in COMBOS:
        if t == "sad":
            D = sad_matrix(positions, order, w, n)
        else:
            D, sigmas, smax, smin, rank = mode_match_matrix(positions, order, w, n)
            sigmas_ref = (sigmas, smax, smin, rank)
        matrices[f"{t}_{w}_{n}"] = matrix_to_list(D)

    sigmas, smax, smin, rank = sigmas_ref
    kappa = condition_number(sigmas)

    fixture = {
        "name": name,
        "positions": [[float(x), float(y), float(z)] for (x, y, z) in positions],
        "designOrder": order,
        "numSpeakers": L,
        "singularValues": [float(sv) for sv in sigmas],
        "sigmaMax": float(smax),
        "sigmaMin": float(smin),
        "conditionNumber": (float(kappa) if kappa != mp.inf else None),
        "effectiveRank": rank,
        "matrices": matrices,
    }

    # rV/rE for the store-default decoder (SAD, maxRe, energy) at spot directions
    default_D = sad_matrix(positions, order, "maxRe", "energy")
    spots = [(0, 0), (37, 0), (100, 0)]
    if name != "ring24":
        spots += [(0, 45), (60, 20)]
    fixture["rvre"] = {
        "decoder": "sad_maxRe_energy",
        "samples": [rv_re(default_D, positions, az, el, order) for (az, el) in spots],
    }

    # ring analytics (horizontal sources). Two robust properties:
    #  * SAD basic: rV DIRECTION error is exactly 0 (regular rig localizes
    #    correctly); its magnitude is a fixture constant (~1.25, NOT 1 -- 3D SH
    #    on the equator with (2l+1) sampling weights is not the flat 2D Dirichlet
    #    kernel), pinned as golden.
    #  * mode-matching basic (rank-truncated on the coplanar ring): reconstructs
    #    the in-plane field, so it is velocity-matched -> ||rV|| = 1, dir err 0.
    if name == "ring24":
        Dsad = sad_matrix(positions, order, "basic", "amplitude")
        ring_sad = [rv_re(Dsad, positions, az, 0, order) for az in (0, 7.5, 33.1)]
        for rc in ring_sad:
            if (rc["rvDirErrorDeg"] or 0) > 1e-9:
                raise SystemExit(f"ring SAD rV direction self-check FAILED: {rc}")
        Dmm, *_ = mode_match_matrix(positions, order, "basic", "amplitude")
        ring_mm = [rv_re(Dmm, positions, az, 0, order) for az in (0, 7.5, 33.1)]
        for rc in ring_mm:
            if abs(rc["rvMagnitude"] - 1.0) > 1e-9 or (rc["rvDirErrorDeg"] or 0) > 1e-9:
                raise SystemExit(f"ring mode-match rV velocity self-check FAILED: {rc}")
        fixture["ringRvBasicSad"] = ring_sad
        fixture["ringRvBasicModeMatch"] = ring_mm

    # Tikhonov config (ring only): mode-match basic energy, lambda_rel = 1e-2
    if name == "ring24":
        Dtik, _s, _mx, _mn, _rk = mode_match_matrix(positions, order, "basic", "energy",
                                                    lam_rel=mp.mpf("1e-2"))
        fixture["tikhonov"] = {"config": "modeMatch_basic_energy_lambda1e-2",
                               "lambdaRel": 1e-2, "matrix": matrix_to_list(Dtik)}
    return fixture, orders


def main():
    mp.mp.dps = DPS

    fixtures = []
    ring_f, _ = build_fixture("ring24", ring24())
    dome_f, _ = build_fixture("dome24", dome24())
    icosa_f, orders2 = build_fixture("icosahedron12", icosahedron12())

    # Self-check 1: SAD == mode-matching on the icosahedron (spherical 5-design)
    # at N=2, all combos, to 25 digits.
    ic_pos = icosahedron12()
    for (w, n) in [("basic", "energy"), ("maxRe", "amplitude")]:
        Dsad = sad_matrix(ic_pos, 2, w, n)
        Dmm, _s, _mx, _mn, _rk = mode_match_matrix(ic_pos, 2, w, n)
        err = max(abs(Dsad[s, c] - Dmm[s, c]) for s in range(Dsad.rows) for c in range(Dsad.cols))
        if err > mp.mpf(10) ** -25:
            raise SystemExit(f"icosa SAD==mode-match self-check FAILED ({w},{n}): {err}")

    # Self-check 2: ring24 full-3D order 3 is rank 7 with huge kappa.
    if ring_f["effectiveRank"] != 7:
        raise SystemExit(f"ring rank self-check FAILED: {ring_f['effectiveRank']}")
    if ring_f["conditionNumber"] is not None and ring_f["conditionNumber"] < 1e12:
        raise SystemExit("ring kappa self-check FAILED (expected huge/inf)")

    # Self-check 3: icosahedron kappa == 1 (perfect design).
    if abs(icosa_f["conditionNumber"] - 1.0) > 1e-10:
        raise SystemExit(f"icosa kappa self-check FAILED: {icosa_f['conditionNumber']}")

    # Self-check 4: dome24 measured rank/kappa are reported (NOT asserted to a
    # specific value -- a hemisphere is honestly ill-conditioned; whatever the
    # designer produces is pinned by the C++ D6 test). Require only that it is
    # full-rank at order 3 (so mode-matching is usable, not a degenerate case
    # like the ring) -- if this ever fails, lower the dome order or add speakers.
    print(f"  dome24: designOrder={dome_f['designOrder']} rank={dome_f['effectiveRank']}"
          f"/{(dome_f['designOrder']+1)**2} kappa={dome_f['conditionNumber']}")
    if dome_f["effectiveRank"] != (dome_f["designOrder"] + 1) ** 2:
        raise SystemExit(f"dome rank self-check FAILED: {dome_f['effectiveRank']} "
                         f"(want {(dome_f['designOrder']+1)**2}); adjust dome geometry/order")

    fixtures = [ring_f, dome_f, icosa_f]

    doc = {
        "provenance": {
            "script": "tools/reference/gen_decoder_reference.py",
            "command": ".venv/Scripts/python tools/reference/gen_decoder_reference.py",
            "python": sh.PYTHON_VERSION, "mpmath": sh.MPMATH_VERSION,
            "generatedUtc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "conventions": "SN3D/ACN bus in, rows=speakers; SAD D0=(1/L)(2l+1)Y^SN3D; "
                           "modeMatch=truncatedPinv(Y^N3D)*diag(g)*diag(sqrt(2l+1)); "
                           "amplitude norm sum_s D[s][W]=1, energy norm sphere-mean E=1",
            "references": ["Zotter & Frank, Ambisonics (Springer 2019) ch.4"],
            "rankToleranceRel": 1e-9,
            "testTolerance": 1e-12,
        },
        "fixtures": fixtures,
    }

    out_path = os.path.abspath(os.path.join(
        os.path.dirname(__file__), "..", "..", "tests", "data", "decoder_reference.json"))
    with open(out_path, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, indent=1)
        fh.write("\n")
    print(f"wrote {out_path} ({len(fixtures)} fixtures; self-checks passed)")


if __name__ == "__main__":
    main()
