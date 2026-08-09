"""Generate the golden SH->binaural decoder bank for the WP15 tests.

requires: python >= 3.12, mpmath == 1.3.0

Regenerate:
    .venv\\Scripts\\python tools\\reference\\gen_binaural_reference.py

Output: tests/data/binaural_reference.json

What this pins
--------------
The sampling ("virtual speaker") SH projection implemented in
Source/DSP/AmbiBinauralDecoder.cpp:

    h_ear[c][n] = (2 l_c + 1)/(4pi) * sum_g w_g * Y_c(d_g) * hrir_ear(d_g)[n]

with Y_c real SN3D/ACN (shref_common, the same conversion the WP3/WP4
generators use) and w_g the exact solid angle of each grid cell. The formula
is implemented here FROM THE SPEC, not transcribed from the C++, so the two
are independent derivations of the same law.

A real SOFA file is deliberately NOT read here: libmysofa is a C library and
this generator stays mpmath-only, like every other one in this directory.
Instead the test fixture is a SYNTHETIC HRIR grid defined below in closed
form, which both languages can build exactly. That keeps the golden about the
decoder math (projection weights, the ACN/SN3D basis, the azimuth handedness
flip, ITD restoration) rather than about SOFA parsing, which the fixture-gated
XoaSofaSmokeTests already covers.

Conventions that this file and the C++ MUST agree on
----------------------------------------------------
* Grid: 72 azimuths x 19 elevations, matching spatcore's HrirDatabase bake.
  Grid azimuth is the SOFA/spatcore head frame: 0 = front, POSITIVE = the
  listener's RIGHT. Elevation is -90..+90, positive up.
* XOA's SH basis measures azimuth counter-clockwise with +Y = LEFT, so the
  basis is evaluated at xoa_az = -grid_az. This negation is the classic
  silent error; it lives here and in gridAzToXoaAzDeg().
* Cell weights are exact zone areas, dAz * (sin(elHi) - sin(elLo)), with the
  pole rows treated as half-height caps shared across the azimuth samples.
  They sum to exactly 4pi.
* Ear index 0 = LEFT, 1 = RIGHT (spatcore's HrirDatabase layout).
* relDelaySec follows the loader contract: the EARLIER ear holds 0 and the
  other holds the ITD, both >= 0.
"""

import datetime
import importlib.metadata
import json
import math
import os
import sys

import mpmath as mp

import shref_common as sh

ORDER = 10
NUM_CHANNELS = (ORDER + 1) ** 2

NUM_AZ = 72
NUM_EL = 19
AZ_STEP_DEG = 360.0 / NUM_AZ
EL_STEP_DEG = 180.0 / (NUM_EL - 1)

SAMPLE_RATE = 48000.0
HRIR_LENGTH = 8          # short on purpose: the golden is about the projection
SINC_HALF = 16           # must match kSincHalf in AmbiBinauralDecoder.cpp
# Constant lead on every filter, both ears alike: the windowed sinc rings
# BACKWARD by SINC_HALF too, and clipping that pre-ring at tap 0 would lose
# energy asymmetrically between a near and a far ear. Matches
# kBinauralFilterLeadSamples in AmbiBinauralDecoder.h.
FILTER_LEAD = SINC_HALF

# -- the synthetic HRIR fixture ------------------------------------------------
# Deterministic closed forms, chosen to exercise everything the decoder must
# get right: a direction-dependent gain that is asymmetric left/right (so a
# sign error in the azimuth flip shows up), elevation dependence (so the el
# axis cannot be ignored), and a real ITD (so dropping relDelaySec shows up).

MAX_ITD_SEC = 0.0007     # ~0.7 ms, a plausible human maximum

TAP_SHAPE = [1.0, 0.5, -0.25, 0.125, -0.0625, 0.03125, -0.015625, 0.0078125]


def ear_gain(grid_az_deg, grid_el_deg, ear):
    """Head-shadow-ish gain in [0.2, 1.0].

    The ear axis points to the listener's left (ear 0) or right (ear 1); grid
    azimuth is positive to the RIGHT, so the right ear's lobe peaks at +90.
    """
    az = math.radians(grid_az_deg)
    el = math.radians(grid_el_deg)
    ear_sign = -1.0 if ear == 0 else 1.0
    # Component of the direction along the ear axis.
    lateral = math.sin(az) * math.cos(el) * ear_sign
    return 0.6 + 0.4 * lateral


def itd_seconds(grid_az_deg, grid_el_deg):
    """Signed ITD: positive means the source is to the RIGHT, so the LEFT ear
    is the later one."""
    az = math.radians(grid_az_deg)
    el = math.radians(grid_el_deg)
    return MAX_ITD_SEC * math.sin(az) * math.cos(el)


def rel_delays(grid_az_deg, grid_el_deg):
    """(left, right) delays in seconds, earlier ear at 0 (loader contract)."""
    itd = itd_seconds(grid_az_deg, grid_el_deg)
    if itd >= 0.0:
        return itd, 0.0          # source to the right: left ear later
    return 0.0, -itd


def hrir_taps(grid_az_deg, grid_el_deg, ear):
    g = ear_gain(grid_az_deg, grid_el_deg, ear)
    return [g * t for t in TAP_SHAPE]


# -- quadrature ----------------------------------------------------------------
def cell_solid_angle(el_index):
    """Exact solid angle of one grid cell (see the module docstring)."""
    step = math.radians(EL_STEP_DEG)
    el = math.radians(-90.0 + el_index * EL_STEP_DEG)
    lo = max(-math.pi / 2, el - step / 2)
    hi = min(math.pi / 2, el + step / 2)
    return (math.sin(hi) - math.sin(lo)) * 2.0 * math.pi / NUM_AZ


# -- fractional delay ----------------------------------------------------------
def add_delayed(src, delay_samples, dst):
    """Blackman-windowed-sinc fractional delay, matching the C++ kernel."""
    int_delay = int(math.floor(delay_samples))
    frac = delay_samples - int_delay

    if frac < 1.0e-9:
        for i, v in enumerate(src):
            j = i + int_delay
            if 0 <= j < len(dst):
                dst[j] += v
        return

    kernel = []
    total = 0.0
    for k in range(-SINC_HALF, SINC_HALF + 1):
        x = k - frac
        s = 1.0 if abs(x) < 1.0e-12 else math.sin(math.pi * x) / (math.pi * x)
        t = ((k + SINC_HALF) - frac) / (2.0 * SINC_HALF)
        w = 0.42 - 0.5 * math.cos(2.0 * math.pi * t) + 0.08 * math.cos(4.0 * math.pi * t)
        kernel.append(s * w)
        total += s * w
    if abs(total) > 1.0e-12:
        kernel = [k / total for k in kernel]

    for i, v in enumerate(src):
        if v == 0.0:
            continue
        base = i + int_delay
        for k in range(-SINC_HALF, SINC_HALF + 1):
            j = base + k
            if 0 <= j < len(dst):
                dst[j] += v * kernel[k + SINC_HALF]


# -- the design ----------------------------------------------------------------
def design():
    max_delay = 0.0
    for az_i in range(NUM_AZ):
        for el_i in range(NUM_EL):
            az_deg = az_i * AZ_STEP_DEG
            el_deg = -90.0 + el_i * EL_STEP_DEG
            l, r = rel_delays(az_deg, el_deg)
            max_delay = max(max_delay, l, r)

    fir_length = HRIR_LENGTH + int(math.ceil(max_delay * SAMPLE_RATE)) + 2 * SINC_HALF + 1

    # channel_gain[c] = (2 l + 1) / 4pi
    channel_gain = []
    for l in range(ORDER + 1):
        for _ in range(-l, l + 1):
            channel_gain.append((2.0 * l + 1.0) / (4.0 * math.pi))

    firs = [[[0.0] * fir_length for _ in range(NUM_CHANNELS)] for _ in range(2)]
    weight_sum = 0.0

    for az_i in range(NUM_AZ):
        az_deg = az_i * AZ_STEP_DEG
        # The handedness flip: spatcore head frame (+right) -> XOA (+left).
        xoa_az_rad = math.radians(-az_deg)
        for el_i in range(NUM_EL):
            el_deg = -90.0 + el_i * EL_STEP_DEG
            basis = sh.real_sh_vector(ORDER, xoa_az_rad, math.radians(el_deg))

            w = cell_solid_angle(el_i)
            weight_sum += w

            delays = rel_delays(az_deg, el_deg)
            for ear in range(2):
                taps = hrir_taps(az_deg, el_deg, ear)
                delayed = [0.0] * fir_length
                add_delayed(taps, delays[ear] * SAMPLE_RATE + FILTER_LEAD, delayed)

                for c in range(NUM_CHANNELS):
                    scale = w * channel_gain[c] * basis[c]
                    if scale == 0.0:
                        continue
                    dst = firs[ear][c]
                    for n in range(fir_length):
                        dst[n] += scale * delayed[n]

    if abs(weight_sum - 4.0 * math.pi) > 1.0e-9:
        sys.exit(f"quadrature weights sum to {weight_sum}, expected 4pi")

    return fir_length, firs


def main():
    fir_length, firs = design()

    out = {
        "_comment": (
            "Golden SH->binaural sampling decode (WP15/D51) over a SYNTHETIC HRIR "
            "grid defined in tools/reference/gen_binaural_reference.py. Consumed by "
            "tests/XoaBinauralDecoderTests.cpp, which rebuilds the same fixture as a "
            "spatcore HrirDatabase and runs Source/DSP/AmbiBinauralDecoder. Tolerance: "
            "1e-5 (the C++ accumulates in double and stores float)."
        ),
        "provenance": {
            "generator": "tools/reference/gen_binaural_reference.py",
            "generated": datetime.datetime.now(datetime.timezone.utc)
                                 .strftime("%Y-%m-%dT%H:%M:%SZ"),
            "python": sys.version.split()[0],
            "mpmath": importlib.metadata.version("mpmath"),
            "formula": "h_ear[c][n] = (2l+1)/(4pi) * sum_g w_g * Y_c(d_g) * hrir_ear(d_g)[n]",
            "conventions": (
                "ACN/SN3D real SH, no Condon-Shortley; grid azimuth positive to the "
                "listener's RIGHT, negated into XOA's +Y-left soundfield frame; ear 0 = "
                "LEFT; exact zone-area quadrature weights summing to 4pi; ITD restored "
                "from relDelaySec with a Blackman-windowed sinc (half-width 16)"
            ),
        },
        "fixture": {
            "numAz": NUM_AZ,
            "numEl": NUM_EL,
            "sampleRate": SAMPLE_RATE,
            "hrirLength": HRIR_LENGTH,
            "maxItdSec": MAX_ITD_SEC,
            "tapShape": TAP_SHAPE,
            "gainFormula": "0.6 + 0.4 * sin(az)*cos(el) * (ear == 1 ? +1 : -1)",
            "itdFormula": "maxItdSec * sin(az) * cos(el); positive -> LEFT ear later",
        },
        "order": ORDER,
        "numChannels": NUM_CHANNELS,
        "firLength": fir_length,
        "tolerance": 1e-5,
        # Full 2 x 121 x firLength bank, ear-major then channel-major.
        "firs": firs,
    }

    path = os.path.abspath(os.path.join(
        os.path.dirname(__file__), "..", "..", "tests", "data", "binaural_reference.json"))
    with open(path, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)
    print(f"wrote {path}: 2 x {NUM_CHANNELS} x {fir_length} taps")


if __name__ == "__main__":
    main()
