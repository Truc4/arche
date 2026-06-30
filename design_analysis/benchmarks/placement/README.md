# Derived-placement benchmarks (Slice 4)

CPU/GPU placement is **derived** from a pure map's signature (`kind==MAP && !eff` ⇒ GPU-eligible) + a
per-machine cost profile (`arche calibrate`, cached in `ARCHE_CACHE_DIR`), decided at build time and frozen
— no runtime scheduler. These benches measure whether that decision is *right* versus explicit flags, and
where the GPU actually wins.

Generate: `python3 gen.py` (writes the two `.arche` files; rerun to change N/K/intensity in `gen.py`).

## `derived_placement.arche` — decision showcase (no annotations)
Two unannotated maps (memory-bound, compute-heavy). See the derived decision:
```
ARCHE_PLACE_DEBUG=1 ARCHE_CACHE_DIR=<dir> arche build --gpu -o /tmp/dp derived_placement.arche
```
Gated as `make test-placement` (under a balanced synthetic profile: derives membound→CPU, heavy→GPU).

## `intensity_sweep.arche` — decision correctness vs measured time
CPU arm + forced-`@gpu` arm per arithmetic intensity, each timed over 20 dispatches; prints
`ROW <flops/elem> <cpu_ms> <gpu_ms>`.
```
arche build --gpu -o /tmp/is intensity_sweep.arche && ARCHE_GPU_DEBUG=1 /tmp/is
```
Measured (GTX 1650, 262,144 rows, non-resident):

| flops/elem | CPU ms | forced-GPU ms | optimal | derived (calibrated) | explicit `@gpu` |
|---|---|---|---|---|---|
| ~1 (membound) | 1 | 296 | CPU | CPU ✓ | GPU ✗ |
| 16 | 2 | 149 | CPU | CPU ✓ | GPU ✗ |
| 32 | 5 | 148 | CPU | CPU ✓ | GPU ✗ |
| 64 | 16 | 149 | CPU | CPU ✓ | GPU ✗ |

GPU time is flat (~150–300 ms = per-dispatch overhead); CPU scales with work. On this box the GPU loses
every single-pass row, and the derived placer correctly keeps them all on the CPU (4/4) where a blunt `@gpu`
is wrong 4/4.

## `resident_loop.arche` — the regime where the GPU wins
`@resident` pool (uploaded once, reused in VRAM) + the same map run K=40 times, so the one-time transfer
amortizes and the GPU's per-flop throughput accumulates.
```
arche build --gpu -o /tmp/rl resident_loop.arche && ARCHE_GPU_DEBUG=1 /tmp/rl
```
Measured (GTX 1650, 16M rows, 40 dispatches): **GPU ≈ 698 ms vs CPU ≈ 1936 ms (~2.8×)** — 40 real GPU
dispatches confirmed.

**Honest status:** Slice 4's cost model is *per-dispatch and non-resident*, so for this kernel it derives
**CPU (wrong here)** — it doesn't yet model "upload once, loop K times." Capturing this win (residency +
amortized-transfer term, and a calibration that records GPU constants instead of declining) is **Slice 5**.
