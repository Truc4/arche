# Placement benchmarks

CPU/GPU placement is derived per machine and frozen into the build (see
[`docs/design/static-mapper.md`](../../../docs/design/static-mapper.md)). These benches check whether the
decision is *right* versus hand-annotated `@gpu`, and reproduce the numbers in that doc. Measured on a
GTX 1650.

Generate the sources: `python3 gen.py`.

## Where the GPU loses — single-pass kernels (`intensity_sweep.arche`)

A CPU arm and a forced-`@gpu` arm per arithmetic intensity, timed over 20 dispatches:

```
arche build --gpu -o /tmp/is intensity_sweep.arche && ARCHE_GPU_DEBUG=1 /tmp/is
```

| flops/elem | CPU ms | forced-GPU ms | optimal | derived | blunt `@gpu` |
|---|---|---|---|---|---|
| ~1 (membound) | 1 | 296 | CPU | CPU ✓ | GPU ✗ |
| 16 | 2 | 149 | CPU | CPU ✓ | GPU ✗ |
| 32 | 5 | 148 | CPU | CPU ✓ | GPU ✗ |
| 64 | 16 | 149 | CPU | CPU ✓ | GPU ✗ |

GPU time is flat (~150–300 ms of per-dispatch overhead); CPU scales with work. The GPU loses every single-pass
row, and the derived placer keeps them all on the CPU (4/4) where a blunt `@gpu` is wrong 4/4. `make
test-placement` gates this under a balanced synthetic profile.

## Where the GPU wins — a resident loop (`derive.arche`)

One compute-heavy `map` (no annotation) run 40× over a `@resident` pool, so the one-time upload amortizes and
the GPU's throughput accumulates. `measure.py` builds it both ways, times each, and freezes the winner:

```
python3 measure.py derive.arche --cache <dir>     # writes <dir>/placement.decisions
arche build --gpu -o /tmp/d derive.arche          # reads the frozen decision
```

The tool times **only-GPU ≈ 696 ms vs all-CPU ≈ 1920 ms → GPU**, freezes `heavy gpu`, and a subsequent plain
`arche build` (no annotation) runs at ≈ 700 ms — a **2.7× win derived by measurement**, source untouched. A
static cost model gets this wrong (it predicts CPU): it can't see the kernel's effective throughput or the
transfer amortizing across the loop. `resident_loop.arche` is the same workload with explicit CPU and GPU arms
that print the comparison for humans.

## Regression

`make test-derived-gpu` — a no-`@gpu` map forced onto the GPU must produce a real `gpu dispatch` (it must
embed the shader for a derived placement, not only for `@gpu`-annotated maps), using an explicit `@gpu`
fixture as the device-presence oracle so it fails rather than skips when a device is present.
