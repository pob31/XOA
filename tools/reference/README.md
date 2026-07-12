# tools/reference — golden-data generators

Reference ("golden") data for the unit tests in `tests/` is **committed** to
the repo, and so is the code that produced it. The tests themselves stay
dependency-free C++ (`tests/XoaTests.cpp` CHECK pattern); Python (scipy/numpy
**or mpmath**, pinned per script) is needed only when *regenerating* data,
never to build or run the tests. The WP3 generators use mpmath 1.3.0 (pure
Python, arbitrary precision — ~40-digit references make golden rounding error
negligible next to the C++ tolerance budget). Local setup:

```
py -3.14 -m venv .venv
.venv\Scripts\python -m pip install mpmath==1.3.0
.venv\Scripts\python tools\reference\gen_sh_reference.py
```

## The convention

- Golden data lives in `tests/data/*.json`, one file per concern
  (SH values, rotation matrices, decoder matrices, NFC curves, …).
- Every data file is produced by a generator script in this directory.
  A data file without a committed generator does not get merged.
- Each script pins its dependencies in the header comment
  (`# requires: python >= 3.11, numpy == x.y, scipy == x.y`) and prints the
  versions it ran with into the JSON output (`"provenance"` field), together
  with the formula/publication it implements.
- Convention conversions live **in the generator, documented in code** — not
  in the consuming test. The canonical example (DEVPLAN WP3): scipy's
  `sph_harm` returns complex SH *with* Condon–Shortley phase; the conversion
  to real ACN/SN3D (AmbiX, no CS phase) is the classic silent-error spot, so
  it is written once, next to the code that does it.
- Regeneration is always a single documented command, e.g.
  `python tools/reference/gen_sh_reference.py > tests/data/sh_reference.json`.
- Tolerances used by the consuming tests are stated in the data file header
  (double-precision design math: typically 1e-10…1e-12).

## Generators (DEVPLAN §9)

All of the below are committed and runnable; "Arrives with" records the work
package each landed in.

| Script | Produces | Arrives with |
|---|---|---|
| `gen_sh_reference.py` | real-SH values (ACN/SN3D, no CS phase) | WP3 |
| `gen_sh_quadrature.py` | Gauss-Legendre×azimuth quadrature grid (orthonormality test; exact for band-20 products) | WP3 |
| `gen_maxre_weights.py` | exact max-rE + in-phase weights, orders 1–10 | WP3 |
| `gen_fuma_reference.py` | FuMa→AmbiX conversion table (from the maxN rule) + test vectors | WP3 |
| `gen_rotation_reference.py` | real-SH rotation blocks via exact quadrature projection (GL(11)×24; independent of Ivanic–Ruedenberg) | WP4 |
| `gen_decoder_reference.py` | SAD/mode-matching golden matrices (ring + dome fixtures) | WP5 |
| `gen_tdesign_tables.py` | virtual-layout t-design (Womersley symmetric, t=33, 564 pts; emits `Source/DSP/TDesignTables.h` + `tdesign_data.json`; source cached as `ss033.00564`) | WP7 |
| `gen_bessel_roots.py` | reverse-Bessel polynomial roots ℓ = 1…10 | WP8 |
| `gen_nfc_reference.py` | NFC magnitude curves (Daniel) across SR/radius | WP8 |
