# offline-render baselines

Each `<machine>.json` here is a committed SHA-256 baseline of the harness output
for one machine. A baseline maps `cpu/<scenario>` to the lowercase hex digest of
the render's raw float32 PCM (all output channels, channel-major, little-endian
— the same bytes `sha256sum` would see on the `--raw` dump).

## Why per-machine, why Release

Float results depend on the compiler and CPU, so a digest is only comparable
against a baseline produced by the **same toolchain on the same machine**, and
only under the **pinned Release flags** (`/Ox /fp:precise`, applied via
`spatcore_apply_compile_flags` — the same flags spatcore's bit-exactness gates
were baselined with). Debug output will not match a Release baseline. This is the
WFS-DIY convention (see `Documentation/XOA-DEVPLAN.md` §6); promoting to shared
per-OS CI baselines waits until cross-machine FP stability is proven.

CI therefore does **not** run `--check`. It builds the harness and runs the
ctest smoke (`--scenario all --blocks 40`), which only proves the harness builds,
runs, and produces non-silent output on all three OSes.

## Developer gate (before pushing a DSP change)

Build Release, then check against your machine's baseline:

```
cmake --build build --config Release --target xoa-offline-render
build/tools/validation/offline-render/xoa-offline-render_artefacts/Release/xoa-offline-render \
    --scenario all --check baselines/<machine>.json
```

Exit 0 = bit-identical. A mismatch means the DSP output changed: if the change
was **intentional**, re-baseline deliberately and commit the new digest with the
change —

```
xoa-offline-render --scenario all --check baselines/<machine>.json --update
```

(`--update` merges into the `--check` file, so digests for scenarios you did not
render this run are preserved.) Name the file after your machine (e.g.
`win-dev.json`); the operator chooses the path, there is no hostname
auto-detection.

## Scenarios

| key                 | scene order | rotation                | exercises                     |
|---------------------|-------------|-------------------------|-------------------------------|
| `cpu/static3`       | 3           | identity                | gather + SAD decode (null anchor) |
| `cpu/rotate`        | 3           | swept yaw + pitch wobble| rotation crossfade transitions |
| `cpu/order-adapt`   | 10          | identity                | FR-7 order adaptation (content 1/3/10) |
| `cpu/scene10`       | 10          | fixed 30° yaw           | full 121-ch rotate + decode GEMM |
