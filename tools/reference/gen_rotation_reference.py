"""Generate golden real-SH rotation matrices for the WP4 tests.

requires: python >= 3.12, mpmath == 1.3.0

Regenerate:
    .venv\\Scripts\\python tools\\reference\\gen_rotation_reference.py

Output: tests/data/rotation_reference.json  (~0.5 MB; runs for several minutes
at high precision -- a one-shot committed generator, progress printed per
rotation).

Method: EXACT QUADRATURE PROJECTION, deliberately independent of the
Ivanic-Ruedenberg recursion the C++ under test implements:

    R^l_{m,m'} = sum_i  w_i * Y^N3D_{l,m}(R d_i) * Y^N3D_{l,m'}(d_i)

over the Golub-Welsch GL(11) x 24-azimuth grid (weights normalized to
sum(w) = 1). Exactness: Y_{l,m}(R d) as a function of d is itself a degree-l
spherical harmonic (rotation preserves each degree-l subspace), so every
summand is a product of two harmonics of degree <= 10 -- band <= 20. Azimuthal
frequency <= 20 < 24 => the uniform azimuth rule is exact; after azimuth
integration the sin(el)-dependence is a polynomial of degree <= 20 <= 2*11-1
=> GL(11) is exact. With N3D factors (orthonormal under sum(w)=1) the
projection isolates exactly R^l_{m,m'} from Y_{l,m}(R d) = sum_k R^l_{m,k}
Y_{l,k}(d). Blocks are identical for SN3D and N3D (per-degree scalar
conjugation).

Conventions carried by this file (and therefore pinned onto the C++):
  * ACTIVE rotation: Y_vec(R d) = blocks * Y_vec(d) (self-checked below).
  * Quaternion (w,x,y,z), right-handed, active; Hamilton product with
    R(q1*q2) = R(q1) R(q2) (q2 applied first).
  * YPR intrinsic Z-Y'-X'': R = Rz(yaw) Ry(pitch) Rx(roll), right-hand rule,
    degrees, frame +X front / +Y left / +Z up.
"""

import datetime
import json
import math
import os
import random

import mpmath as mp

import shref_common as sh

ORDER = 10
GL_NODES = 11
N_AZIMUTH = 24
DPS = 30
CHECK_TOL_EXP = -24   # self-check tolerance 1e-24


def acn(l, m):
    return l * (l + 1) + m


# -- rotation conventions (documented in module banner) -------------------------

def quat_multiply(q1, q2):
    """Hamilton product q1*q2 (q2 applied first): R(q1*q2) = R(q1)R(q2)."""
    w1, x1, y1, z1 = q1
    w2, x2, y2, z2 = q2
    return (
        w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2,
        w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2,
        w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2,
        w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2,
    )


def axis_quat(axis, angle_deg):
    half = mp.radians(mp.mpf(angle_deg)) / 2
    s = mp.sin(half)
    return (mp.cos(half), s * axis[0], s * axis[1], s * axis[2])


def ypr_to_quat(yaw_deg, pitch_deg, roll_deg):
    qz = axis_quat((mp.mpf(0), mp.mpf(0), mp.mpf(1)), yaw_deg)
    qy = axis_quat((mp.mpf(0), mp.mpf(1), mp.mpf(0)), pitch_deg)
    qx = axis_quat((mp.mpf(1), mp.mpf(0), mp.mpf(0)), roll_deg)
    return quat_multiply(quat_multiply(qz, qy), qx)


def quat_to_matrix(q):
    """Unit-quaternion -> 3x3 active rotation (column-vector convention)."""
    n = mp.sqrt(sum(c * c for c in q))
    w, x, y, z = (c / n for c in q)
    return [
        [1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
        [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
        [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)],
    ]


def ypr_to_matrix(yaw_deg, pitch_deg, roll_deg):
    """Rz(yaw) * Ry(pitch) * Rx(roll), right-hand rule."""
    g = mp.radians(mp.mpf(yaw_deg))
    b = mp.radians(mp.mpf(pitch_deg))
    a = mp.radians(mp.mpf(roll_deg))
    cg, sg = mp.cos(g), mp.sin(g)
    cb, sb = mp.cos(b), mp.sin(b)
    ca, sa = mp.cos(a), mp.sin(a)
    rz = [[cg, -sg, 0], [sg, cg, 0], [0, 0, 1]]
    ry = [[cb, 0, sb], [0, 1, 0], [-sb, 0, cb]]
    rx = [[1, 0, 0], [0, ca, -sa], [0, sa, ca]]

    def matmul(p, q):
        return [[sum(p[i][k] * q[k][j] for k in range(3)) for j in range(3)] for i in range(3)]

    return matmul(rz, matmul(ry, rx))


def mat_apply(m, v):
    return tuple(sum(m[i][j] * v[j] for j in range(3)) for i in range(3))


# -- grid + basis ---------------------------------------------------------------

def build_grid():
    nodes, node_weights = sh.gauss_legendre_nodes_weights(GL_NODES)
    points = []   # (cartesian tuple, weight)
    for x, wx in zip(nodes, node_weights):
        el = mp.asin(mp.mpf(x))
        ce = mp.cos(el)
        for j in range(N_AZIMUTH):
            az = -mp.pi + 2 * mp.pi * j / N_AZIMUTH
            d = (ce * mp.cos(az), ce * mp.sin(az), mp.mpf(x))
            w = wx * (2 * mp.pi / N_AZIMUTH) / (4 * mp.pi)
            points.append((d, w))
    total = sum(w for _, w in points)
    assert abs(total - 1) < mp.mpf(10) ** CHECK_TOL_EXP, f"sum(w) = {total}"
    return points


def n3d_vector(d):
    """All 121 N3D SH values at cartesian direction d (unit-ish)."""
    az = mp.atan2(d[1], d[0])
    el = mp.asin(d[2] / mp.sqrt(d[0] ** 2 + d[1] ** 2 + d[2] ** 2))
    out = [mp.mpf(0)] * ((ORDER + 1) * (ORDER + 1))
    for l in range(ORDER + 1):
        scale = mp.sqrt(2 * l + 1)
        for m in range(-l, l + 1):
            out[acn(l, m)] = scale * sh.real_sh_sn3d(l, m, az, el)
    return out


def project_blocks(points, basis, rot_matrix):
    """Blocks R^l via quadrature projection; returns list of (2l+1)^2 lists."""
    n_ch = (ORDER + 1) * (ORDER + 1)
    rotated = []
    for d, _w in points:
        rotated.append(n3d_vector(mat_apply(rot_matrix, d)))

    blocks = []
    for l in range(ORDER + 1):
        n = 2 * l + 1
        base = l * l
        block = [[mp.mpf(0)] * n for _ in range(n)]
        for (d, w), b_row, br_row in zip(points, basis, rotated):
            for i in range(n):
                bri = w * br_row[base + i]
                if bri == 0:
                    continue
                row = block[i]
                for j in range(n):
                    row[j] += bri * b_row[base + j]
        blocks.append(block)
    del rotated

    # self-checks per rotation
    tol = mp.mpf(10) ** CHECK_TOL_EXP
    for l in range(ORDER + 1):
        n = 2 * l + 1
        blk = blocks[l]
        for i in range(n):
            for j in range(n):
                dot = sum(blk[i][k] * blk[j][k] for k in range(n))
                assert abs(dot - (1 if i == j else 0)) < tol, \
                    f"orthogonality failed l={l} ({i},{j}): {dot}"

    # l=1 block == permutation-conjugated cartesian matrix (sigma: -1->y,0->z,+1->x)
    sigma = [1, 2, 0]   # SH index (m+1) -> cartesian axis index
    for i in range(3):
        for j in range(3):
            expected = rot_matrix[sigma[i]][sigma[j]]
            assert abs(blocks[1][i][j] - expected) < tol, \
                f"l=1 seed mismatch ({i},{j}): {blocks[1][i][j]} vs {expected}"

    det1 = (blocks[1][0][0] * (blocks[1][1][1] * blocks[1][2][2] - blocks[1][1][2] * blocks[1][2][1])
            - blocks[1][0][1] * (blocks[1][1][0] * blocks[1][2][2] - blocks[1][1][2] * blocks[1][2][0])
            + blocks[1][0][2] * (blocks[1][1][0] * blocks[1][2][1] - blocks[1][1][1] * blocks[1][2][0]))
    assert abs(det1 - 1) < tol, f"det(l=1) = {det1}"

    return blocks


def active_convention_check(blocks, rot_matrix, rng):
    """Pin the ACTIVE convention into the goldens: Y_vec(R d) == blocks * Y_vec(d)."""
    tol = mp.mpf(10) ** CHECK_TOL_EXP
    for _ in range(2):
        z = mp.mpf(2 * rng.random() - 1)
        az = mp.mpf(360 * rng.random() - 180)
        el = mp.degrees(mp.asin(z))
        d = (mp.cos(mp.radians(el)) * mp.cos(mp.radians(az)),
             mp.cos(mp.radians(el)) * mp.sin(mp.radians(az)), z)
        y_d = n3d_vector(d)
        y_rd = n3d_vector(mat_apply(rot_matrix, d))
        for l in range(ORDER + 1):
            n = 2 * l + 1
            base = l * l
            for i in range(n):
                acc = sum(blocks[l][i][j] * y_d[base + j] for j in range(n))
                assert abs(acc - y_rd[base + i]) < tol, \
                    f"active-convention check failed l={l} row={i}"


def main():
    mp.mp.dps = DPS
    rng = random.Random(20260707)

    points = build_grid()
    print(f"grid: {len(points)} points, sum(w)=1 ok")
    basis = [n3d_vector(d) for d, _w in points]
    print("unrotated basis computed")

    named = [
        ("identity", (0.0, 0.0, 0.0)),
        ("yaw90", (90.0, 0.0, 0.0)),
        ("pitch90", (0.0, 90.0, 0.0)),
        ("roll90", (0.0, 0.0, 90.0)),
    ]
    randoms = []
    for k in range(6):
        yaw = 360.0 * rng.random() - 180.0
        pitch = math.degrees(math.asin(2.0 * rng.random() - 1.0))
        roll = 360.0 * rng.random() - 180.0
        randoms.append((f"random-{k}", (yaw, pitch, roll)))

    tol = mp.mpf(10) ** CHECK_TOL_EXP
    entries = []
    quats = {}
    for name, (yaw, pitch, roll) in named + randoms:
        q = ypr_to_quat(yaw, pitch, roll)
        m_from_q = quat_to_matrix(q)
        m_from_ypr = ypr_to_matrix(yaw, pitch, roll)
        for i in range(3):
            for j in range(3):
                assert abs(m_from_q[i][j] - m_from_ypr[i][j]) < tol, \
                    f"{name}: quaternion vs YPR matrix mismatch ({i},{j})"

        blocks = project_blocks(points, basis, m_from_ypr)
        active_convention_check(blocks, m_from_ypr, rng)

        if name == "identity":
            for l in range(ORDER + 1):
                n = 2 * l + 1
                for i in range(n):
                    for j in range(n):
                        assert abs(blocks[l][i][j] - (1 if i == j else 0)) < tol, \
                            "identity blocks are not identity"

        quats[name] = q
        entries.append({
            "name": name,
            "yawPitchRollDeg": [yaw, pitch, roll],
            "quaternion": [float(c) for c in q],
            "cartesian": [[float(m_from_ypr[i][j]) for j in range(3)] for i in range(3)],
            "blocks": [[float(v) for row in blocks[l] for v in row] for l in range(ORDER + 1)],
        })
        print(f"projected + self-checked: {name}")

    # composition entries: quaternion + cartesian only (blocks come from the
    # block-bearing entries; C++ R4 composes those)
    compositions = [("compose-r0-r1", "random-0", "random-1"),
                    ("compose-yaw90-pitch90", "yaw90", "pitch90")]
    for name, a, b in compositions:
        q12 = quat_multiply(quats[a], quats[b])
        m12 = quat_to_matrix(q12)
        ma = quat_to_matrix(quats[a])
        mb = quat_to_matrix(quats[b])
        prod = [[sum(ma[i][k] * mb[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
        for i in range(3):
            for j in range(3):
                assert abs(m12[i][j] - prod[i][j]) < tol, f"{name}: composition matrix mismatch"
        entries.append({
            "name": name,
            "composedFrom": [a, b],
            "quaternion": [float(c) for c in q12],
            "cartesian": [[float(m12[i][j]) for j in range(3)] for i in range(3)],
        })
        print(f"composed: {name}")

    doc = {
        "provenance": {
            "script": "tools/reference/gen_rotation_reference.py",
            "command": ".venv/Scripts/python tools/reference/gen_rotation_reference.py",
            "python": sh.PYTHON_VERSION,
            "mpmath": sh.MPMATH_VERSION,
            "generatedUtc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "method": "quadrature projection R^l_mm' = sum_i w_i Y^N3D_lm(R d_i) Y^N3D_lm'(d_i), "
                      "GL(11)x24 grid, exact for band-20 products; deliberately independent of "
                      "Ivanic-Ruedenberg (1996; erratum 1998), which is what the C++ under test implements",
            "conventions": "active rotation Y(R d) = R^l Y(d); blocks identical for SN3D and N3D; "
                           "quaternion w,x,y,z Hamilton active, R(q1*q2)=R(q1)R(q2); "
                           "YPR intrinsic Z-Y'-X'' = Rz(yaw)Ry(pitch)Rx(roll), RH, degrees; "
                           "frame +X front +Y left +Z up",
            "seed": 20260707,
            "testTolerance": 1e-12,
        },
        "order": ORDER,
        "rotations": entries,
    }

    out_path = os.path.abspath(os.path.join(
        os.path.dirname(__file__), "..", "..", "tests", "data", "rotation_reference.json"))
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")
    print(f"wrote {out_path} ({len(entries)} entries)")


if __name__ == "__main__":
    main()
