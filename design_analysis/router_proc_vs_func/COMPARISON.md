# Router: procedural (module-static) vs reentrant (out-param) — an honest comparison

Two implementations of the same segment-radix URL router, identical trie and resolution logic,
differing only in **how captured `:param`/`*catchall` spans are reported** back to the caller.
Handler ids are a `route` enum in both (not raw ints).

- **Procedural** (`bench_proc.arche`, the current `stdlib/router` design): `resolve(path)(handler)`
  records captures into **module-global** arrays (`cap_start`/`cap_len`/`cap_count`); the caller
  reads them back via `param(i)` / `param_count()`.
- **Reentrant** (`bench_func.arche`): `resolve(path)(starts, lens, handler, count)` writes captures
  into **caller-allocated out-params** — no per-request module state. (Loosely "functional": it's
  reentrant and side-effect-free *across calls*, but it still mutates the out-buffers in place and
  shares the global trie pool. Pure-functional — immutable, fold-builds-a-new-result — is
  impossible here: it needs heap-allocated persistent structures or value-aggregate copy-out, and
  Arche has neither. So the honest property is **reentrancy**, achieved by ownership of the output
  buffers, not purity.)

Both build with the same compiler/flags and were validated to produce an **identical checksum
(20000000)** over 2,000,000 resolutions of `/users/42/posts/99` against the route set
`/`, `/users/:id`, `/users/:id/posts/:pid`, `/static/*` — i.e. they resolve identically.

Segment matching uses **sub-slices**: `seg_eq(stored, TrieNode.seg_len[child], path[s:e])` passes the
path segment as a borrowed `char[]` view (so the body indexes `seg[i]` from 0, no start offset) and
uses its `seg.length` directly — `char[]` is now a normal `(ptr, len)` array, so no explicit segment
length is threaded. The *stored* segment lives in a fixed `char[32]` column (no length of its own),
so its content length travels in a sibling `seg_len :: int` column. `handler_id`/`add` are typed with
the `route` enum.

## Measured

| Dimension | Procedural | Reentrant (out-param) |
|---|---|---|
| Correctness (checksum) | 20000000 | 20000000 (identical) |
| Throughput, ns/op (best of 5) | ~20.4 | ~21.0 |
| Throughput, ns/op (range) | 20.4–25.4 | 21.0–30.3 |
| LLVM IR lines | 1662 | 1629 |

**Performance is a wash.** The difference is within run-to-run noise. This is expected: in a
no-heap language *both* designs write captures into pre-reserved fixed memory — module `.data`
(procedural) vs the caller's stack buffers (reentrant). Neither allocates; both are direct stores.
The reentrant version passes two extra out-pointer arguments per call, but that cost is invisible
next to the trie walk. IR size is comparable (the reentrant version is slightly *smaller* — it has
no separate `param`/`param_count` accessor procs).

## Where each wins

**Reentrant wins — no shared state.** Captures live in the caller's storage, so nested or
concurrent resolves don't clobber each other. The procedural version's module statics are
overwritten by the *next* `resolve`, so you must read the captures out before resolving again — it
is not reentrant and not thread-safe. (The procedural router's own test, `route_resolve.arche`, has
a comment working around exactly this: it hoists `param(0)` out of a `match` because a second
cross-module out-param call would clobber the state.)

**Call sites are comparable.** Both bind their results positionally:

```arche
resolve(path)(h:);  param(0)(st:, ln:);            // procedural: result, then read captures back
resolve(path)(s0:, l0:, h:, c:);                   // reentrant: captures bound as out-params
```

The reentrant form binds the capture buffers as **out-params** (allocated by the binding, named
once) — not the foreign-style in-out shadow (a buffer repeated in the in- and out-lists, which
exists only to line up with a C ABI). One call, one result list; the only cost is declaring the
two span buffers as outputs.

**Memory.** Procedural uses one shared module buffer (8+8+1 ints) for the whole program; the
reentrant form uses per-call-site stack buffers (one set per active resolve). Its footprint scales
with concurrency (as it must, to be reentrant); procedural's is fixed (because it can't be).

## Takeaway

Neither is "faster" — performance is identical. The choice is **semantic**: if the router is a
one-shot, single-threaded convenience, the procedural module-statics form is terser. If it must be
reentrant / used concurrently (a real web server), the out-param form is the correct design and
costs nothing at runtime. It is enabled by non-`char` array out-params (and `own`/in-out array
params) — which required the array-param codegen fixes logged in `DECISIONS.md`. It is *not* pure
functional; the win is reentrancy, not purity.

## Reproduce

```sh
arche build -o /tmp/bp design_analysis/router_proc_vs_func/bench_proc.arche && /tmp/bp
arche build -o /tmp/bf design_analysis/router_proc_vs_func/bench_func.arche && /tmp/bf
```
