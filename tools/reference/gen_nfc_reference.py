"""Generate NFC magnitude-response goldens for the WP8 tests (FR-6).

requires: python >= 3.12, mpmath == 1.3.0

Regenerate:
    .venv\\Scripts\\python tools\\reference\\gen_nfc_reference.py

Output: tests/data/nfc_reference.json

For each (sample rate, r_ref, r_src, order l) case this emits two magnitude
curves over a log frequency grid:

  * magAnalogDb  - the physical Daniel curve |H_l(jw)| in dB, straight from the
                   pole/zero product H_l(s) = prod_i (s - q_i c/r_src)/(s - q_i c/r_ref).
                   This is the "match Daniel reference curves" target.
  * magDigitalDb - the realized digital filter |H_l(e^{jwT})| in dB, built by the
                   SAME bilinear transform (K = 2 Fs, no prewarp) the C++
                   designSourceSections() uses. A digital-to-digital comparison
                   pins the C++ coefficient math exactly; a digital-to-analog
                   comparison (loose near Nyquist, to absorb bilinear warp)
                   confirms the physical shape.

The clamps (kMinSourceRadius, kMaxBoostDb) MUST match Source/DSP/AmbiNFCFilter.h;
each case reports clampedByOrder so the test compares against Daniel only where
the clamp is inactive. Speed of sound MUST match xoa::kSpeedOfSound.
"""

import datetime
import json
import os
import sys

import mpmath as mp

import gen_bessel_roots as bessel   # same directory; provides roots_of_theta

_REQUIRED_MPMATH = "1.3.0"
try:
    import importlib.metadata as _md
    _ACTUAL_MPMATH = _md.version("mpmath")
except Exception:  # pragma: no cover
    _ACTUAL_MPMATH = mp.__version__
if _ACTUAL_MPMATH != _REQUIRED_MPMATH:
    sys.exit(f"gen_nfc_reference: mpmath {_REQUIRED_MPMATH} required, found {_ACTUAL_MPMATH}.")

mp.mp.dps = 40
PYTHON_VERSION = sys.version.split()[0]

# MUST match Source/XoaConstants.h and Source/DSP/AmbiNFCFilter.h.
C = mp.mpf("343.0")
K_MIN_RADIUS = mp.mpf("0.25")
K_MAX_BOOST_DB = mp.mpf("20.0")

SAMPLE_RATES = [44100, 48000, 96000]
R_REF = [mp.mpf("1.0"), mp.mpf("2.0")]
R_SRC = [mp.mpf("0.5"), mp.mpf("1.0"), mp.mpf("4.0")]
ORDERS = list(range(1, 11))
NUM_FREQS = 32

# Tolerances the consuming C++ test uses (documented here, per README convention).
TOL = {
    "digitalDb": 1.0e-3,          # digital-vs-digital: pins the C++ bilinear math
    "analogBelowFsOver8Db": 0.15, # digital-vs-Daniel by band (bilinear warp grows
    "analogBelowFsOver4Db": 0.6,  # toward Nyquist)
    "analogAboveFsOver4Db": 2.0,
    "dcGainDb": 1.0e-4,           # exact (r_ref/r_src)^l at DC
}


def clamped_radius(r_src, r_ref, l):
    floor = r_ref * mp.mpf(10) ** (-K_MAX_BOOST_DB / (20 * l))
    return max(r_src, K_MIN_RADIUS, floor)


def analog_mag_db(l, roots, r_src_eff, r_ref, f):
    """|H_l(j2pi f)| in dB from the pole/zero product over all l roots."""
    s = mp.mpc(0, 2 * mp.pi * f)
    h = mp.mpc(1)
    for q in roots:
        z = q * C / r_src_eff
        p = q * C / r_ref
        h *= (s - z) / (s - p)
    return 20 * mp.log10(abs(h))


def bilinear_sections(l, roots, r_src_eff, r_ref, sr):
    """Digital biquad/1st-order coefficients for order l - mirrors the C++
    designSourceSections() exactly (K = 2 Fs, no prewarp)."""
    K = 2 * mp.mpf(sr)
    secs = []
    reals = [q for q in roots if abs(mp.im(q)) < mp.mpf(10) ** -20]
    pairs = [q for q in roots if mp.im(q) > mp.mpf(10) ** -20]  # one per conj pair

    for q in pairs:
        zr, zi = mp.re(q) * C / r_src_eff, mp.im(q) * C / r_src_eff
        pr, pi = mp.re(q) * C / r_ref,     mp.im(q) * C / r_ref
        az, mz = zr, zr * zr + zi * zi
        ap, mp_ = pr, pr * pr + pi * pi
        da0 = K * K - 2 * ap * K + mp_
        secs.append((
            (K * K - 2 * az * K + mz) / da0,
            (2 * (mz - K * K)) / da0,
            (K * K + 2 * az * K + mz) / da0,
            (2 * (mp_ - K * K)) / da0,
            (K * K + 2 * ap * K + mp_) / da0,
        ))
    for q in reals:
        zr = mp.re(q) * C / r_src_eff
        pr = mp.re(q) * C / r_ref
        d = K - pr
        secs.append(((K - zr) / d, -(K + zr) / d, mp.mpf(0), -(K + pr) / d, mp.mpf(0)))
    return secs


def digital_mag_db(secs, f, sr):
    w = 2 * mp.pi * mp.mpf(f) / mp.mpf(sr)
    e1 = mp.exp(mp.mpc(0, -w))
    e2 = mp.exp(mp.mpc(0, -2 * w))
    h = mp.mpc(1)
    for (b0, b1, b2, a1, a2) in secs:
        num = b0 + b1 * e1 + b2 * e2
        den = 1 + a1 * e1 + a2 * e2
        h *= num / den
    return 20 * mp.log10(abs(h))


def log_freqs(sr):
    lo, hi = mp.mpf(20), mp.mpf("0.45") * sr
    return [lo * (hi / lo) ** (mp.mpf(i) / (NUM_FREQS - 1)) for i in range(NUM_FREQS)]


def main():
    roots_by_order = {l: bessel.roots_of_theta(l) for l in ORDERS}
    cases = []

    for sr in SAMPLE_RATES:
        freqs = log_freqs(sr)
        for r_ref in R_REF:
            for r_src in R_SRC:
                for l in ORDERS:
                    r_eff = clamped_radius(r_src, r_ref, l)
                    clamped = abs(r_eff - r_src) > mp.mpf(10) ** -20
                    roots = roots_by_order[l]
                    secs = bilinear_sections(l, roots, r_eff, r_ref, sr)

                    mag_analog = [float(analog_mag_db(l, roots, r_eff, r_ref, f)) for f in freqs]
                    mag_digital = [float(digital_mag_db(secs, f, sr)) for f in freqs]
                    dc_gain_db = float(20 * l * mp.log10(r_ref / r_eff))

                    cases.append({
                        "sampleRate": sr,
                        "rRef": float(r_ref),
                        "rSrc": float(r_src),
                        "order": l,
                        "clampedByOrder": bool(clamped),
                        "dcGainDb": dc_gain_db,
                        "freqs": [float(f) for f in freqs],
                        "magAnalogDb": mag_analog,
                        "magDigitalDb": mag_digital,
                    })

    doc = {
        "provenance": {
            "script": "tools/reference/gen_nfc_reference.py",
            "command": ".venv/Scripts/python tools/reference/gen_nfc_reference.py",
            "python": PYTHON_VERSION,
            "mpmath": _ACTUAL_MPMATH,
            "generatedUtc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "definitions": "H_l(s)=prod_i (s - q_i c/r_src)/(s - q_i c/r_ref); "
                           "q_i = reverse Bessel roots; digital via bilinear K=2Fs, no prewarp; "
                           "DC gain (r_ref/r_src)^l. Bass-boost (audible) direction; see "
                           "gen_bessel_roots.py for the sign derivation.",
            "references": ["Daniel (2003), Spatial sound encoding including near field effect"],
        },
        "speedOfSound": float(C),
        "clamp": {"minSourceRadius": float(K_MIN_RADIUS), "maxBoostDb": float(K_MAX_BOOST_DB)},
        "tolerances": TOL,
        "cases": cases,
    }

    out_path = os.path.abspath(os.path.join(
        os.path.dirname(__file__), "..", "..", "tests", "data", "nfc_reference.json"))
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")
    print(f"wrote {out_path} ({len(cases)} cases, {NUM_FREQS} freqs each)")


if __name__ == "__main__":
    main()
