"""Shared reference machinery for the XOA spherical-harmonics generators.

requires: python >= 3.12, mpmath == 1.3.0

This module owns THE documented complex -> real SH conversion (per
tools/reference/README.md: "convention conversions live in the generator,
documented in code"). It is imported by gen_sh_reference.py and
gen_fuma_reference.py so the conversion exists in exactly one place.

Convention produced: ACN ordering, SN3D normalization, NO Condon-Shortley
phase (AmbiX). Frame matches Source/Helpers/XoaCoordinates.h:
  +X front, +Y left, +Z up; azimuth CCW from +X; elevation from the horizon.

On import this module runs a self-check against closed-form SN3D expressions
(AmbiX paper, Nachbar et al. 2011) and aborts if the conversion is wrong, so a
bad reference can never be written to disk.
"""

import importlib.metadata
import math
import sys

import mpmath as mp

# -- pinned-version gate -------------------------------------------------------
_REQUIRED_MPMATH = "1.3.0"
_ACTUAL_MPMATH = importlib.metadata.version("mpmath")
if _ACTUAL_MPMATH != _REQUIRED_MPMATH:
    sys.exit(
        f"shref_common: mpmath {_REQUIRED_MPMATH} required, found {_ACTUAL_MPMATH}. "
        f"Use the pinned .venv (pip install mpmath=={_REQUIRED_MPMATH})."
    )

mp.mp.dps = 40  # ~40 significant digits; double rounding is the only reference error

PYTHON_VERSION = sys.version.split()[0]
MPMATH_VERSION = _ACTUAL_MPMATH


def acn(l, m):
    """ACN channel index for order l, degree m."""
    return l * (l + 1) + m


def gauss_legendre_nodes_weights(n):
    """Gauss-Legendre nodes and weights for integral over [-1, 1] via
    Golub-Welsch (eigenvalues of the symmetric tridiagonal Jacobi matrix).

    Robust to any n (no seed-convergence risk): nodes are the eigenvalues,
    weights are 2 * (first eigenvector component)^2. sum(weights) == 2 exactly.
    Returns (nodes, weights) as lists of mpmath reals, nodes ascending.
    """
    a = mp.zeros(n)
    for k in range(1, n):
        b = mp.mpf(k) / mp.sqrt(4 * k * k - 1)
        a[k - 1, k] = b
        a[k, k - 1] = b
    evals, evecs = mp.eigsy(a)
    pairs = sorted(((evals[i], 2 * evecs[0, i] ** 2) for i in range(n)),
                   key=lambda p: p[0])
    nodes = [p[0] for p in pairs]
    weights = [p[1] for p in pairs]
    return nodes, weights


def real_sh_sn3d(l, m, az_rad, el_rad):
    """Real SH Y_{l,m} in ACN/SN3D, no Condon-Shortley, as an mpmath real.

    mpmath.spherharm(l, m, theta, phi) is the physics-convention complex
    spherical harmonic, orthonormal on the sphere and *including* the
    Condon-Shortley phase (-1)^m. We convert in three explicit, separable
    steps:

      theta = pi/2 - el   (colatitude), phi = az
      m == 0:  Y =                       sqrt(4pi/(2l+1)) * spherharm(l, 0, theta, phi)
      m  > 0:  Y = (-1)^m * sqrt(2) *     sqrt(4pi/(2l+1)) * Re[spherharm(l,  m, theta, phi)]
      m  < 0:  Y = (-1)^|m| * sqrt(2) *   sqrt(4pi/(2l+1)) * Im[spherharm(l, |m|, theta, phi)]

    Factor by factor:
      * (-1)^|m|          removes the Condon-Shortley phase that spherharm includes.
      * sqrt(4pi/(2l+1))  rescales orthonormal -> SN3D.
      * sqrt(2)  (m != 0) rebuilds the real cos/sin partner from the complex one.
    """
    theta = mp.pi / 2 - mp.mpf(el_rad)
    phi = mp.mpf(az_rad)
    am = abs(m)

    orthonormal_to_sn3d = mp.sqrt(mp.mpf(4) * mp.pi / (2 * l + 1))
    cs_removal = mp.mpf(-1) ** am  # undo Condon-Shortley

    if m == 0:
        return orthonormal_to_sn3d * mp.re(mp.spherharm(l, 0, theta, phi))

    ylm = mp.spherharm(l, am, theta, phi)
    real_factor = mp.sqrt(2) * orthonormal_to_sn3d * cs_removal
    if m > 0:
        return real_factor * mp.re(ylm)
    return real_factor * mp.im(ylm)


def real_sh_vector(order, az_rad, el_rad):
    """All (order+1)^2 SN3D coefficients in ACN order, as Python floats."""
    out = [0.0] * ((order + 1) * (order + 1))
    for l in range(order + 1):
        for m in range(-l, l + 1):
            out[acn(l, m)] = float(real_sh_sn3d(l, m, az_rad, el_rad))
    return out


# -- self-check: closed forms from the AmbiX paper (Nachbar et al. 2011) --------
def _closed_form_l2(az, el):
    """The 9 SN3D channels through order 2, closed form. Returns dict acn->value."""
    ca, sa = mp.cos(az), mp.sin(az)
    ce, se = mp.cos(el), mp.sin(el)
    s3 = mp.sqrt(3)
    return {
        0: mp.mpf(1),                       # W
        1: ce * sa,                         # Y  (1,-1)
        2: se,                              # Z  (1, 0)
        3: ce * ca,                         # X  (1,+1)
        4: (s3 / 2) * ce * ce * mp.sin(2 * az),   # V (2,-2)
        5: (s3 / 2) * mp.sin(2 * el) * sa,        # T (2,-1)
        6: (3 * se * se - 1) / 2,                 # R (2, 0)
        7: (s3 / 2) * mp.sin(2 * el) * ca,        # S (2,+1)
        8: (s3 / 2) * ce * ce * mp.cos(2 * az),   # U (2,+2)
    }


def _self_check():
    directions = [
        (0.3, 0.4), (1.1, -0.7), (2.9, 0.1), (-1.5, 1.2), (0.0, 0.0),
    ]
    tol = mp.mpf(10) ** -30
    for az, el in directions:
        closed = _closed_form_l2(az, el)
        for idx, expected in closed.items():
            l = int(math.isqrt(idx))
            m = idx - l * (l + 1)
            got = real_sh_sn3d(l, m, az, el)
            if abs(got - expected) > tol:
                sys.exit(
                    f"shref_common self-check FAILED: acn={idx} (l={l},m={m}) "
                    f"az={az} el={el}: got {got}, expected {expected}"
                )


_self_check()
