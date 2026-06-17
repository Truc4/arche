# Open items

Unaddressed work around `map` / collectives / `@gpu`. Line numbers are approximate (they drift).
Reference docs for shipped behavior: `docs/language.md` (§Maps, §Collectives, §GPU maps).

## Sort — DONE (index-permutation + dispatch-by-key-type)

`sort(Pool.key)` was an O(n²·k) all-columns-inline insertion sort. Replaced (`emit_sort()` +
helpers in `codegen/codegen.c`) with: build an `i64` index permutation, sort the indices by the key
(**LSD radix** for integer-shaped keys, **iterative heapsort** for `float`/`i128`, **insertion** for
n<32), then apply it to every column once via **in-place cycle-following**. O(n·k) data moves; all
scratch is compile-time-sized `alloca` (every pool is static).

- [x] Index-permutation + per-column apply (cycle-following, not a temp buffer — allocas hoist to entry,
      so a per-column temp would be always-on O(N·k) stack).
- [x] Heapsort (iterative, no recursion) for float/i128; LSD radix (1–8 passes by width) for integers.
- [x] Insertion small-n fast path (<32) — existing n=4/5 fixtures stay byte-identical.
- [x] Stable: index tie-break in the comparator (heapsort/insertion); radix is naturally stable.
- [x] **Fixed a latent bug**: unsigned keys (`u8…u128`) used a signed compare; now `ult/ugt`.
- [x] Fixtures added (`tests/unit/language/collectives/sort_*`): large-n radix, float heapsort, heap +
      radix stability, unsigned, i8/i64 radix widths, multicolumn cycle-apply.
- [x] Verified: 763/763 tests; ~**300× faster** on n=20000 reverse (0.878s → ~0.002s); unsigned fixture
      red-on-old / green-on-new. Benchmark: `design_analysis/benchmarks/sort/`.
- Staged: radix for `float` (bit-flip transform) and `i128` (currently heapsort); SIMD/GPU sort.

## GPU coverage — the limits are in the emitter/dispatch site, not the runtime

`runtime/gpu_runtime.c` is already type- and count-agnostic (N typed buffers by base pointer + push
constants). The "single static all-float pool" scope lives in `codegen/gpu_glsl.c` and the dispatch
site `codegen/codegen.c:~7474-7557`. Steps 1–3 alone erase the entire "single static float pool" limit
with **zero runtime change**.

- [ ] **Typed emitter + typed dispatch** (int / mixed columns). Hardcoded `float` in 3 places:
      `arch_matches_float` (`gpu_glsl.c:~207`), the dispatch guard (`codegen.c:~7491`), the SSBO decl
      `float %s[]` (`gpu_glsl.c:~239`); plus the literal float-izer (`gpu_glsl.c:~112`) and the
      hardcoded `elem_size` `i32 4` (`codegen.c:~7539`). Runtime already takes `elem_size`.
- [ ] **Multi-pool dispatch** — drop `matching_count == 1` (`codegen.c:~7480`); dispatch once per
      matching pool, reusing the CPU path's loop.
- [ ] **Heap / dynamic pools** — emit the column-base GEP from the runtime pool pointer instead of the
      static-global GEP (`codegen.c:~7506`); drop the static-capacity guard (`codegen.c:~7484`).
- [ ] **Consts / singletons → push constants / uniforms** (the push-constant plumbing already exists).
- [ ] **Pure `func` inlining** into the kernel body (replaces the `select`-only special case).
- [ ] **Operators** `%`, `and`, `or` (currently bail in `binop_glsl` / the emitter).
- [ ] **Diagnostics** — replace the single silent skip with: compile **error** if `@gpu` is on a map
      that's inexpressible in principle; **warning** if expressible-but-unimplemented; **silent** only
      for the no-device / no-toolchain case (a machine fact, not a program defect).

## f32 — code defects + precision honesty

`float` is f32 on both CPU and GPU; the unification is complete on the load-bearing paths. Loose ends:

- [ ] Monoid `min`/`max` identities are emitted as **f64** bit-patterns into f32 slots
      (`codegen.c:~2089-2092`) — correct only by coincidence (±inf round-trips exactly). Use f32
      patterns; a future finite identity written this way would corrupt silently.
- [ ] Make `double` a **hard error** until a real f64 type exists — it's currently a silent f32 alias
      (`lower/lower.c:~77`, `codegen.c:~2551`), i.e. a precision trap.
- [ ] Delete the dead f64 `Vec3` prelude (`codegen.c:~9722`) and the unreachable, self-contradicting
      `strcmp(...,"float")` branch (`codegen.c:~9813-9816`, comment says "float is a 64-bit double").
- [ ] `align 8` on a 4-byte float store (`codegen.c:~7118`) → `align 4`.
- [ ] **Precision claim:** CPU == GPU is bit-for-bit only on *contraction-free* kernels. FMA
      contraction (GPU may fuse `a*b+c`, CPU doesn't) and dual decimal→f32 literal rounding can each
      differ by ~1 ULP. Either emit GPU outputs `precise` / disable contraction to actually deliver
      bit-for-bit, or keep the "≤1 ULP otherwise" caveat. (The CPU fallback is still always safe —
      CPU is the reference.)
- [ ] Possible: an f64 accumulator option for `reduce`/`scan` (f32 storage, f64 fold) for
      accumulation-heavy use, without changing the f32 default.

## Staged (designed, not built)

- [ ] **GPU runtime performance** — device-local buffers, persistent column residency across `run`s,
      async submit + barriers, non-blocking `run`. Shipped path is host-coherent buffers
      created+freed per dispatch + synchronous `vkQueueWaitIdle`. Correctness-first; this is pure speed.
- [ ] **`scan` vectorization** — still scalar; needs Hillis-Steele / blocked prefix scan.
- [ ] **Collectives on the GPU** — `reduce`/`scan` tree-fold / segmented scan, `sort` as a GPU sort.
- [ ] **Instanced rendering** — bind SoA columns as an instance buffer, one instanced draw, instead of
      CPU software raster.
- [ ] **User-defined monoids** — a `func` + explicit identity (the internal identity table already has
      the shape); plus `and`/`or` reduce ops.
- [ ] **Sort extensions** — comparator / multi-key (the all-columns-together invariant is fixed).
