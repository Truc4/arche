# Router: procedural vs functional (ownership-threaded) — an honest comparison

Two implementations of the same segment-radix URL router, identical trie and resolution logic,
differing only in **how captured `:param`/`*catchall` spans are reported** back to the caller:

- **Procedural** (`bench_proc.arche`, the current `stdlib/router` design): `resolve(path)(handler)`
  records captures into **module-global** arrays (`cap_start`/`cap_len`/`cap_count`); the caller
  reads them back via `param(i)` / `param_count()`.
- **Functional / ownership-threaded** (`bench_func.arche`): `resolve(path, cstart, clen)(cstart,
  clen, handler, count)` writes captures into the **caller's** in-out buffers and returns the count.
  No per-request module state.

Both build with the same compiler/flags and were validated to produce an **identical checksum
(18000000)** over 2,000,000 resolutions of `/users/42/posts/99` against the route set
`/`, `/users/:id`, `/users/:id/posts/:pid`, `/static/*` — i.e. they resolve identically.

## Measured

| Dimension | Procedural | Functional |
|---|---|---|
| Correctness (checksum) | 18000000 | 18000000 (identical) |
| Throughput, ns/op (best of 5) | ~20.4 | ~21.0 |
| Throughput, ns/op (range) | 20.4–25.4 | 21.0–30.3 |
| LLVM IR lines | 1662 | 1629 |

**Performance is a wash.** The difference is within run-to-run noise. This is expected: in a
no-heap language *both* designs write captures into pre-reserved fixed memory — module `.data`
(procedural) vs the caller's stack buffers (functional). Neither allocates; both are direct stores.
The functional version passes two extra pointer arguments per call, but that cost is invisible next
to the trie walk. IR size is comparable (the functional version is slightly *smaller* — it has no
separate `param`/`param_count` accessor procs).

## Where each wins

**Functional wins — reentrancy / no shared state.** Captures live in the caller's storage, so
nested or concurrent resolves don't clobber each other. The procedural version's module statics are
overwritten by the *next* `resolve`, so you must read the captures out before resolving again — it
is not reentrant and not thread-safe. (The procedural router's own test, `route_resolve.arche`, has
a comment working around exactly this: it hoists `param(0)` out of a `match` because a second
cross-module out-param call would clobber the state.)

**Procedural wins — call-site ergonomics.** The procedural call site is terser:

```arche
resolve(path)(h:);  param(0)(st:, ln:);          // procedural
```
```arche
s0: int[8]; l0: int[8];                            // functional: caller declares + threads buffers
resolve(path, s0, l0)(s0, l0, h:, c:);
```

The caller must allocate and thread the capture buffers in the functional form. For one-shot use
that is more ceremony; for a server handling many requests it is the correct, reentrant shape.

**Memory.** Procedural uses one shared module buffer (8+8+1 ints) for the whole program; functional
uses per-call-site stack buffers (one set per active resolve). Functional's footprint scales with
concurrency (as it must, to be reentrant); procedural's is fixed (because it can't be).

## Takeaway

Neither is "faster" — performance is identical. The choice is **semantic**: if the router is a
one-shot, single-threaded convenience, the procedural module-statics form is terser. If it must be
reentrant / used concurrently (a real web server), the functional ownership-threaded form is the
correct design and costs nothing at runtime. The threading is enabled by treating capture buffers
as `own`/in-out arrays — which required the array-param codegen fixes logged in `DECISIONS.md`.

## Reproduce

```sh
arche build -o /tmp/bp design_analysis/router_proc_vs_func/bench_proc.arche && /tmp/bp
arche build -o /tmp/bf design_analysis/router_proc_vs_func/bench_func.arche && /tmp/bf
```
