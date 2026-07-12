"""Generate reverse-Bessel-polynomial roots for the WP8 NFC filters (FR-6).

requires: python >= 3.12, mpmath == 1.3.0

Regenerate:
    .venv\\Scripts\\python tools\\reference\\gen_bessel_roots.py

Output: Source/DSP/AmbiNfcTables.h  (GENERATED — do not edit by hand).

------------------------------------------------------------------------------
What these roots are, and why
------------------------------------------------------------------------------
Near-field compensation (Daniel 2003) shapes each Ambisonic order l by the
finite distance of the source. The per-order compensation filter, for a source
at radius r_src reproduced on an array whose mean radius is r_ref, is

    H_l(s) = product_{i=1..l} (s - q_i * c / r_src) / (s - q_i * c / r_ref)

where the q_i are the l roots of the reverse Bessel polynomial theta_l, and c is
the speed of sound. Derivation (documented here so the sign/direction lives in
exactly one place, next to the code that produces the table):

  The exterior radial solution for order l is the spherical Hankel function
      h_l^(2)(x) = (-j)^{l+1} e^{-jx}/x * theta_l(jx)/(jx)^l ,   x = w r / c
  so with s = jw we have jx = s r / c and theta_l(jx) = theta_l(s r / c).
  The physical near-field filter (source at r vs plane wave), after removing the
  common 1/r amplitude and e^{-jx} propagation delay, is
      NF_l(s) = theta_l(s r/c) / (s r/c)^l ,     NF_l -> 1 as s->inf,  -> inf at DC.
  It diverges at DC, so it is never realized alone; NFC divides by the same
  quantity at the array radius r_ref (the array's own re-radiation regularizes
  it):
      H_l(s) = NF_l(s, r_src) / NF_l(s, r_ref)
             = (r_ref/r_src)^l * theta_l(s r_src/c) / theta_l(s r_ref/c) .
  theta_l is MONIC (theta_l(y) = prod_i (y - q_i)), so
      theta_l(s r/c) = (r/c)^l * prod_i (s - q_i c/r),
  and the (r_ref/r_src)^l prefactor cancels the (r/c)^l normalizers exactly,
  leaving the clean pole/zero form at the top of this comment:
      zeros z_i = q_i c/r_src  (move with the source),
      poles p_i = q_i c/r_ref  (depend only on the rig — hence stability is
                                governed entirely by r_ref, fixed per layout).
  DC gain  = prod z_i/p_i = (r_ref/r_src)^l  (proximity bass-boost for near
             sources — this is the quantity the C++ gain-ceiling clamp bounds).
  HF gain  -> 1.

  NOTE ON DIRECTION: XOA-DEVPLAN's WP8 line writes H_l as F_l(s r_ref)/F_l(s r_src),
  which is the RECIPROCAL of the audible convention (it would attenuate bass for
  near sources). The audible/physical direction — bass boost as a source
  approaches — is the one derived above and emitted here. This is the WP8
  analogue of the WP3 Condon-Shortley note: the convention is pinned in the
  generator, documented, and self-checked.

Reverse Bessel polynomial (the one definition everything derives from):
    theta_l(y) = sum_{k=0..l} (l+k)! / (2^k k! (l-k)!) * y^{l-k}      (monic, deg l)
Its l roots all lie strictly in the left half-plane (theta_l is Hurwitz), which
is what makes every pole/zero above stable and minimum-phase for any r > 0.

------------------------------------------------------------------------------
Self-checks (abort on mismatch)
------------------------------------------------------------------------------
  * closed forms: theta_1 = y+1 (root -1); theta_2 = y^2+3y+3 (roots -1.5 +/- j*sqrt(3)/2);
    theta_3 real root -2.3222.
  * residual |theta_l(q)| < 1e-30 at every root.
  * every root strictly in the LHP (Re(q) < 0).
  * conjugate closure: the non-real roots come in exact conjugate pairs.
"""

import datetime
import json
import math
import os
import sys
from math import factorial

import mpmath as mp

_REQUIRED_MPMATH = "1.3.0"
try:
    import importlib.metadata as _md
    _ACTUAL_MPMATH = _md.version("mpmath")
except Exception:  # pragma: no cover
    _ACTUAL_MPMATH = mp.__version__
if _ACTUAL_MPMATH != _REQUIRED_MPMATH:
    sys.exit(f"gen_bessel_roots: mpmath {_REQUIRED_MPMATH} required, found {_ACTUAL_MPMATH}.")

MAX_ORDER = 10
DPS = 50
mp.mp.dps = DPS

PYTHON_VERSION = sys.version.split()[0]


def theta_coeffs(l):
    """Coefficients of theta_l, highest degree first (monic): c_k on y^{l-k}."""
    return [factorial(l + k) // (2 ** k * factorial(k) * factorial(l - k)) for k in range(l + 1)]


def theta_eval(l, y):
    c = theta_coeffs(l)
    acc = mp.mpf(0)
    for coeff in c:            # Horner, highest first
        acc = acc * y + coeff
    return acc


def roots_of_theta(l):
    """The l roots of theta_l, high precision. Returns list of mpmath complex."""
    coeffs = [mp.mpf(c) for c in theta_coeffs(l)]
    r = mp.polyroots(coeffs, maxsteps=200, extraprec=200)
    return list(r)


def canonical_sections(l):
    """Group the l roots into ceil(l/2) sections: conjugate pairs (Im>0
    representative) sorted by ascending |Im|, then the lone real root (odd l)
    last. Each returned tuple is (re, im) with im==0 marking a 1st-order section."""
    roots = roots_of_theta(l)

    # residual + LHP self-checks
    for q in roots:
        if abs(theta_eval(l, q)) > mp.mpf(10) ** -30:
            sys.exit(f"gen_bessel_roots: residual too large, l={l}, q={q}")
        if mp.re(q) >= 0:
            sys.exit(f"gen_bessel_roots: root not in LHP, l={l}, q={q}")

    reals = [q for q in roots if abs(mp.im(q)) < mp.mpf(10) ** -20]
    complexes = [q for q in roots if abs(mp.im(q)) >= mp.mpf(10) ** -20]

    if len(reals) != (l % 2):
        sys.exit(f"gen_bessel_roots: expected {l % 2} real root(s) at l={l}, got {len(reals)}")
    if len(complexes) % 2 != 0:
        sys.exit(f"gen_bessel_roots: odd number of complex roots at l={l}")

    # keep the Im>0 representative of each conjugate pair; verify closure
    pos = sorted((q for q in complexes if mp.im(q) > 0), key=lambda q: abs(mp.im(q)))
    for q in pos:
        if not any(abs(mp.conj(q) - w) < mp.mpf(10) ** -20 for w in complexes if mp.im(w) < 0):
            sys.exit(f"gen_bessel_roots: no conjugate partner for {q} at l={l}")
    if 2 * len(pos) != len(complexes):
        sys.exit(f"gen_bessel_roots: conjugate count mismatch at l={l}")

    sections = [(mp.re(q), mp.im(q)) for q in pos]
    for q in reals:
        sections.append((mp.re(q), mp.mpf(0)))
    assert len(sections) == (l + 1) // 2
    return sections


CLOSED_FORM = {
    1: [(mp.mpf(-1), mp.mpf(0))],
    2: [(mp.mpf(-3) / 2, mp.sqrt(3) / 2)],
}


def main():
    all_sections = []          # flat list of (re, im)
    section_count = [0] * (MAX_ORDER + 1)
    section_offset = [0] * (MAX_ORDER + 1)
    per_order_json = []

    for l in range(1, MAX_ORDER + 1):
        secs = canonical_sections(l)
        if l in CLOSED_FORM:
            for (gre, gim), (ere, eim) in zip(secs, CLOSED_FORM[l]):
                if abs(gre - ere) > mp.mpf("1e-30") or abs(gim - eim) > mp.mpf("1e-30"):
                    sys.exit(f"gen_bessel_roots: closed-form self-check FAILED l={l}")
        section_count[l] = len(secs)
        section_offset[l] = len(all_sections)
        all_sections.extend(secs)
        per_order_json.append({"order": l,
                               "roots": [[float(re), float(im)] for re, im in secs]})

    total = len(all_sections)
    assert total == sum(section_count)

    # cumulative offsets already captured per order; offset[0]=0 (order 0 has none)
    header = build_header(all_sections, section_count, section_offset, total, per_order_json)

    out_path = os.path.abspath(os.path.join(
        os.path.dirname(__file__), "..", "..", "Source", "DSP", "AmbiNfcTables.h"))
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)
    print(f"wrote {out_path} ({total} sections, orders 1..{MAX_ORDER})")


def build_header(sections, count, offset, total, per_order_json):
    stamp = datetime.datetime.now(datetime.timezone.utc).isoformat()
    lines = []
    A = lines.append
    A("#pragma once")
    A("")
    A("//" + "=" * 78)
    A("// XOA - reverse-Bessel-polynomial roots for near-field compensation (WP8,")
    A("// FR-6). GENERATED FILE - regenerate with tools/reference/gen_bessel_roots.py;")
    A("// do not edit by hand.")
    A("//")
    A("// Per Ambisonic order l = 1..10, the compensation filter is")
    A("//   H_l(s) = prod_i (s - q_i c/r_src) / (s - q_i c/r_ref)")
    A("// with q_i the roots of the monic reverse Bessel polynomial theta_l. Roots")
    A("// are grouped into ceil(l/2) sections: a conjugate pair (im>0 representative,")
    A("// -> 2nd-order section) or a lone real root (im==0, -> 1st-order section).")
    A("// The full derivation and the sign/direction convention live in the")
    A("// generator header comment.")
    A("//")
    A(f"// generated: {stamp}")
    A(f"// python: {PYTHON_VERSION}  mpmath: {_ACTUAL_MPMATH}")
    A("//" + "=" * 78)
    A("")
    A("namespace xoa::nfc::tables")
    A("{")
    A("")
    A(f"constexpr int kMaxOrder = {MAX_ORDER};")
    A(f"constexpr int kTotalSections = {total};")
    A("")
    A("/** One reverse-Bessel root per section. im == 0 marks a real root")
    A("    (1st-order section); im > 0 marks a conjugate pair (2nd-order). */")
    A("struct Root { double re; double im; };")
    A("")
    A("/** Number of sections (ceil(l/2)) for each order, index 0..kMaxOrder. */")
    A("constexpr int kSectionCount[kMaxOrder + 1] = { "
      + ", ".join(str(count[i]) for i in range(MAX_ORDER + 1)) + " };")
    A("")
    A("/** Offset of order l's first section into kRoots, index 0..kMaxOrder. */")
    A("constexpr int kSectionOffset[kMaxOrder + 1] = { "
      + ", ".join(str(offset[i]) for i in range(MAX_ORDER + 1)) + " };")
    A("")
    A("constexpr Root kRoots[kTotalSections] = {")
    idx = 0
    for l in range(1, MAX_ORDER + 1):
        n = count[l]
        chunk = sections[offset[l]: offset[l] + n]
        reprs = ", ".join("{ %s, %s }" % (repr(float(re)), repr(float(im))) for re, im in chunk)
        A(f"    {reprs},   // order {l}")
        idx += n
    A("};")
    A("")
    A("} // namespace xoa::nfc::tables")
    A("")
    return "\n".join(lines)


if __name__ == "__main__":
    main()
