# WIP: Portable mapped binaries (`--adaptive`)

Status: **design only, not implemented.** This is a working doc — decisions, blockers, and open questions for
making a built binary carry good CPU/GPU placement and residency to hardware other than the build host.
Reader-facing description of placement lives in `../design/static-mapper.md`; the known-limitation note is
there too.

## The problem

Placement (CPU vs GPU per kernel) and residency (which pools stay in VRAM, where the coherence syncs go) are
**derived once and frozen into the build**, using the build host's machine profile. Ship that binary to a
different machine and the frozen choices can be wrong for it — a GPU left idle, a GPU chosen where it now
loses, a pool kept resident that shouldn't be.

**What already works (do not re-solve):** the frozen binary is still *correct* anywhere. An absent/slower GPU
falls back to the CPU (`arche_gpu_dispatch` returns nonzero → CPU path), and a derived `gpu.sync` on a pool
that isn't actually resident at runtime is a no-op. So the gap is **performance portability**, not
correctness. That is what makes an incremental fix safe.

## Chosen design

A `--adaptive` build flag (NOT the default — plain `arche build` stays single-frozen for the build host):

1. **Bake reasonable defaults from the static estimate.** For each eligible kernel, freeze a default
   CPU/GPU bit from the closed-form cost model (`cg_placement_prefer_gpu`) under a generic profile — the
   answer used when the target has no bench cache.
2. **Emit both forms, gated on a runtime bit.** Guard the GPU dispatch on a per-kernel bit:
   `if (arche_place[id]) { gpu dispatch } else { cpu }`. The CPU fallback path already exists, so this is a
   small codegen change, not a second backend.
3. **Bake a kernel cost table** into the binary: per kernel `(flops_per_elem, rows, ncol)` — the inputs the
   cost model needs, already known at build.
4. **Launch-time selection.** A small `arche_select_placement()` called once from `@main`:
   - bench cache (`machine.profile`) present → run the cost model over the baked table with the cached
     constants, set each `arche_place[id]` bit (accurate per-machine placement);
   - absent → leave the baked estimate defaults.
   This is a table fill, O(1) in the schedule, then the program runs. **No runtime scheduler, no
   benchmarking on the hot path.**
5. **Cache generation is opt-in and already exists.** `arche calibrate` writes `machine.profile` to the cache
   dir today. "Generate the cache on the target" = run it once. For toolchain-less targets, add a
   `--calibrate` self-mode so the shipped binary can produce its own cache in-process.

## Decisions

- **D1 — Flag, not default.** Adaptive binaries cost size + a codegen guard; most builds are build==run.
  Plain `arche build` is unchanged.
- **D2 — Correctness is never at stake, only speed.** Justified by the CPU-fallback + no-op-sync properties
  above. This is why launch-time placement selection is safe against a schedule whose residency was baked from
  the estimate: a kernel flipped CPU→GPU runs non-resident (auto up/download, correct); a kernel flipped
  GPU→CPU leaves a baked sync that no-ops (correct).
- **D3 — Structural probe for the coarse axis, measured cache for the fine axis.** Grounded in the research
  split: CUDA fatbin / glibc IFUNC pick reliably on discoverable identity (GPU present? which ISA?), while
  AutoMap / FFTW argue performance is *not* an analytic function of identity — the fine ranking (which GPU
  tier, does resident-GPU actually win) needs measurement. So: identity decides GPU-present/absent; a measured
  `machine.profile` decides the rest.
- **D4 — Never benchmark on the startup hot path by default.** No shipped tool blocks first launch on a bench
  (ATLAS benches at install; FFTW caches "wisdom" on first *use* and can save/load it; StarPU refines online
  across a long-lived process). Cold cache → use the estimate default now; `arche calibrate`/`--calibrate`
  populates the cache deliberately to upgrade the next launch. Background-bench (StarPU's model) only pays off
  for long-lived / repeatedly-invoked binaries, not one-shot CLIs — deferred, not core.
- **D5 — Reuse existing infra.** The default = the existing static estimate; the cache = the existing
  `machine.profile` format; the generator = the existing `arche calibrate`. Net-new is the codegen guard, the
  baked cost table, and the launch selector.
- **D6 — Placement first, residency second (phasing).** Placement is a per-kernel runtime bit and flips
  correctly at launch (D2). Residency is *schedule structure* (which syncs exist, where) and can't be re-run
  at launch — so residency adaptivity needs a small set of baked whole-schedule variants selected by the same
  cache. Phase 1 ships adaptive placement; Phase 2 adds adaptive residency.

## Blockers / open problems

- **B1 — The cost model must live in the runtime.** `cg_placement_prefer_gpu` + `cg_kernel_flops_per_elem`
  are compiler-side (codegen.c). Launch-time selection needs the cost function (not the derivation) in the
  runtime lib, fed by the baked `(flops_per_elem, rows, ncol)` table. Decide: duplicate the formula into
  `runtime/`, or generate a data-only table the runtime interprets. Keep one source of truth for the formula.
- **B2 — Residency can't be re-decided at launch (D6).** Full residency adaptivity = baking N whole-schedule
  variants (each = frozen `@arche_run` + its embedded shaders) + a selector. Open: how many variants, how the
  developer names the target classes (preset tiers vs `--targets=...` calibrated on a known fleet), and the
  binary-size cost (each variant carries its own SPIR-V — the CUDA fatbin-bloat tradeoff). Not needed for
  Phase 1.
- **B3 — Cache location + trust.** Where does the runtime look for `machine.profile` (env `ARCHE_CACHE_DIR`
  vs an XDG path vs next to the binary), what if it's stale (built for an old GPU, since replaced), and
  read-only / containerized filesystems where no cache can be written. Fallback chain: cache → structural
  probe (GPU present?) → estimate default → CPU floor.
- **B4 — The estimate default's profile.** The baked default needs *some* profile to run the estimate under.
  A generic "typical discrete GPU" profile makes the estimate pick GPU where clearly beneficial but is itself
  the unreliable estimate AutoMap warns against; an all-CPU default is ultra-safe but leaves the GPU idle
  until a cache exists. Leaning conservative (CPU-safe or a deliberately cautious generic profile), since the
  cache is the accurate path.
- **B5 — Toolchain-less cache generation.** `--calibrate` self-mode means the shipped binary embeds the
  calibrator (a tiny GPU probe + CPU FMA timing). Confirm this links without the full toolchain and writes a
  profile the selector can consume.
- **B6 — `measure.py` is still a separate manual step.** The most accurate cache today comes from whole-run
  measurement, not the micro-calibration `arche calibrate` does. Until per-kernel micro-benchmarking is folded
  into calibration, the launch-time selector is only as good as the estimate + coarse profile constants.

## Prior art

- **Structural / feature dispatch:** CUDA fatbin (cubin by compute capability, PTX JIT fallback), glibc IFUNC
  / function-multiversioning (CPUID at load). Reliable for *capability*, not for *speed ranking*.
- **Measure-and-freeze:** ATLAS (install-time), FFTW (`wisdom`, measure-on-first-use, save/load), Qilin
  (adaptive CPU/GPU split trained on the target), TVM/Ansor, AutoMap (SC 2023 — "measure, don't estimate").
- **Online refinement:** StarPU (per-machine history models, calibrated by sampling, refined across runs).
