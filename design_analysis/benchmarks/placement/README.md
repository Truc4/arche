# Placement benchmarks

CPU/GPU placement is derived per machine and frozen into the build (see
[`docs/design/static-mapper.md`](../../../docs/design/static-mapper.md)). These benches check whether the
decision is *right* versus hand-annotated `@gpu`, and reproduce the numbers in that doc. Measured on a
GTX 1650.

> Known limitation: these decisions are frozen for the **build host**. A binary run on other hardware stays
> correct (CPU fallback) but is not re-tuned — portable/adaptive binaries are not supported yet
> ([`docs/wip/portable-mapped-binaries.md`](../../../docs/wip/portable-mapped-binaries.md)).

Generate the sources: `python3 gen.py`.

## The intensity crossover (`intensity_sweep.arche`)

A CPU arm and a forced-`@gpu` arm per arithmetic intensity, each over its **own** pool (so derived
residency/coherence never crosses arms) and timed over 20 dispatches. A warmup dispatch absorbs one-time
device init:

```
arche build --gpu -o /tmp/is intensity_sweep.arche && /tmp/is
```

| flops/elem | CPU ms | GPU ms (resident) | faster |
|---|---|---|---|
| 0 (membound) | ~1 | ~4 | CPU |
| 16 | ~2 | ~3 | CPU (≈tie) |
| 32 | ~6 | ~3 | GPU |
| 64 | ~16 | ~3 | GPU |

GPU time is roughly flat (~3 ms — per-dispatch launch overhead over 20 dispatches, once the pool is resident
and the transfer amortizes); CPU scales with work. So the two cross over around intensity 16–32: the CPU wins
the launch-overhead-bound low end, the GPU wins the compute-bound high end. This is a truer picture than a
non-resident sweep, where every row was transfer-dominated (~150–300 ms) and the GPU appeared to lose
everywhere — derived residency removes that noise.

Catching the high-intensity flip in the *derived decision* (not just the forced arm) needs the cost model to
account for residency amortization — today it does not (it estimates a per-dispatch transfer and stays
conservative), which is the residency-aware measurement noted in
[`static-mapper.md`](../../../docs/design/static-mapper.md) → *Not yet built*. `make test-placement` gates the
decision on the compute-bound `derived_placement.arche` (membound→CPU, heavy→GPU) under a synthetic profile.

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

That win only lands because the pool is kept **resident** (uploaded once, not downloaded each dispatch).
`derive_residency.arche` is `derive.arche` with the `@resident` and `gpu.sync` annotations *removed* — the
coherence pass derives both from the schedule (see [placement](../../../docs/design/static-mapper.md) →
*Residency and coherence*). It reproduces the same **≈1910 ms CPU vs ≈701 ms derived-GPU** with identical
output, source fully un-annotated:

```
ARCHE_FORCE_PLACE=cpu arche build --gpu -o /tmp/cpu derive_residency.arche
ARCHE_FORCE_PLACE=gpu arche build --gpu -o /tmp/gpu derive_residency.arche   # residency + sync derived
```

### Derived vs hand-tuned, head to head

`derive.arche` (hand `@resident` + `gpu.sync`) and `derive_residency.arche` (derived) are now the **same
workload** — same 16M rows, same 40× `heavy`, same `show` observable — differing *only* in whether residency
is annotated or derived. Built the same way and run interleaved (best-of-5, GTX 1650):

```
ARCHE_FORCE_PLACE=gpu arche build --gpu -o /tmp/hand derive.arche
ARCHE_FORCE_PLACE=gpu arche build --gpu -o /tmp/deriv derive_residency.arche
```

| | ms | output |
|---|---|---|
| hand-tuned (`@resident` + `gpu.sync`) | ~678 | `g0=1.2731` |
| derived (no annotations) | ~672 | `g0=1.2731` |

The derived schedule is **within ~1% of hand-tuned** with identical output — it derives the same residency and
sync placement, so it matches rather than beats. (`ARCHE_COH_DEBUG=1` on the derived build prints the
`resident GR` + `sync GR before show` it inserts; on the hand build it prints nothing — the annotations are
honored and not duplicated.)

## Integer columns (`derive_int.arche`)

The GPU path is no longer float-only. `derive_int.arche` is the same shape as `derive_residency.arche` but
over an **`int`** column — a compute map (inlined `int` consts + integer `%`) run 40× over a resident int
pool. It emits an `int` SSBO, keeps the pool resident, and produces a result **bit-identical to the CPU**
(integer arithmetic and truncation/modulo match), while winning by the same amortization: on a GTX 1650,
**CPU ≈ 995 ms vs GPU ≈ 434 ms (~2.3×)**. Only same-typed 32-bit columns are emittable; mixed int/float,
non-32-bit widths, and integer division by a non-constant divisor stay on the CPU.

## Regression

`make test-derived-gpu` — a no-`@gpu` map forced onto the GPU must produce a real `gpu dispatch` (it must
embed the shader for a derived placement, not only for `@gpu`-annotated maps), using an explicit `@gpu`
fixture as the device-presence oracle so it fails rather than skips when a device is present.

`make test-gpu-int` — an `int`-column map (named const + integer division) must dispatch on the GPU and
compute the same truncating-integer result as the CPU (`v0=48 v3=48`), guarding the type-aware emitter (int
SSBO, constant inlining, i32 dispatch). `make test-gpu` also validates the emitted int shader through glslc.

`make test-derived-residency` — a pool written by consecutive GPU maps then read on the host, with no
annotations, must derive both residency and a `gpu.sync` before the host read. It asserts the derivation at
build time (`ARCHE_COH_DEBUG`) and, on a device, that the derived sync is *load-bearing*: with it the host
reads the correct value, and with it suppressed (`ARCHE_COH_NO_SYNC`) the resident pool goes stale.
