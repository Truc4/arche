# Data-oriented io & data-derived scheduling

How effects that need a *resource* (a file, a socket) fit the model: the resource is **data in a pool**, the
operations are **systems**, and ordering is **derived from the data**. This revises one default in
[scheduling.md](scheduling.md) (see "Ordering" below) and gives the io surface a home that isn't a passed
handle. Motivating failure: `drop/autodrop_in_ir` — "close an `fd`" is meaningless because an opened file
should be an owned *resource*, not an `int`/enum.

## Ordering: derived from data, not from position

The default is **independent systems, order derived from the data they touch** — not the written `seq`
order. A system declares the columns it reads and writes (it must, to be a system); a writer of column `C`
runs before readers of `C`. Systems that share no columns are independent and unordered.

- **Deterministic, not a footgun.** Two *data-dependent* systems are ordered by the data → deterministic.
  Two *disjoint* systems have no order, but their relative order is unobservable (that is what disjoint
  means), so the runtime may run them in any order / in parallel. This is not Bevy's "conflicting systems
  unordered" hazard — conflicting (data-sharing) systems are always ordered.
- **Coupling stays honest.** Order-by-position hides coupling in the list ("B happens to follow A"); reorder
  the list and it breaks silently. Order-by-data makes the *only* coupling a data dependency, visible in the
  read/write sets — and free, since you declare those anyway. The common `input → sim → render` pipeline
  orders itself, because it *is* a data pipeline.
- **`seq` / position** demotes to a grouping/readability convenience, not the ordering mechanism.

### `system X depends on Y` — the explicit, enforced edge

For an **effect-ordering** dependency the data graph cannot see (e.g. flush a write buffer *before* close),
declare it: `X depends on Y` (surface form TBD, e.g. `@after(Y)` on the system). It is a *declared*,
*checkable* constraint — the compiler rejects a schedule that cannot honor it, unlike a `seq` position that
reorders silently. General primitive, not io-specific; io mostly gets its edges for free (below).

> Revises `scheduling.md`: that doc says "order is what you wrote (`seq`)". The default becomes "order is
> what the data implies; `seq`/`depends on` are overrides." `par` remains the explicit concurrency opt-in.

## io as pool-resident data

A resource is a row, not a value you pass:

- **`File` is a pool-resident archetype.** `open` inserts a `File` row carrying the kernel fd + state. The
  row exists or it doesn't; there is no descriptor to "close."
- **Operations apply to rows, via queues.** To read, insert a request row (a `ReadReq` pool referencing the
  `File` by entity handle); a `read` system drains the queue, performs the syscall, and writes results back
  (a `ReadResult` pool/column). Same command-buffer pattern as `insert`/`delete` and event pools — io is
  systems-over-pools, on the flat-effect spine (effects in systems, data in pools).
- **Synchronous by ordering, not by blocking.** `seq({ enqueue_read, drain_read, consume })` is synchronous
  within one pass: by the time `consume` runs, the read already happened in `drain`. And the queues create
  the edges automatically — `enqueue` writes `ReadReq`, `drain` reads it → drain-after-enqueue for free;
  `drain` writes `ReadResult`, `consume` reads it → consume-after-drain for free. No async machinery; no
  mmap-vs-stream split (both are just systems the schedule sequences).
- **close = the row leaving the pool.** Deleting the `File` row fires the kernel close — a `@drop`-on-delete
  hook, the analogue of RAII but tied to the row's lifecycle, not lexical scope. (`autodrop_in_ir`'s
  scope-exit premise is *replaced* by this: delete the row → close emitted.)
- **Passing a handle stays allowed** as an escape hatch, but it buys nothing — operating on a row is strictly
  easier than threading a handle — so there is no hard datasheet seal forcing it; the pool is the natural
  model.

## What this unblocks

`autodrop_in_ir` is reworked under this model (delete the `File` row → close in the IR), not satisfiable as
written (scope-exit RAII on an `fd` enum). It stays red until io-as-data lands.

## Status / scope

Implementation is phased and separate: (1) data-derived ordering as the default + `depends on`;
(2) io-as-data (`File`/request/result pools, `@drop`-on-delete); (3) rework `autodrop_in_ir`. The `fd` enum
remains for the three standard streams (stdin/stdout/stderr), which are never opened or closed.

**Landed so far:** the `@drop`-on-delete *primitive* — a `@drop(<Archetype>)` destructor fires when a pool
row is `delete`d, receiving the dying row's columns by value (e.g. `@drop(File) close :: proc(fd: i64)` →
`close(row.fd)` emitted inside `@arche_delete_File` at the validated slot). This is the close-on-row-delete
hook from "io as pool-resident data" above. **Still open in phase (2):** the actual `File`/request/result
pool surface in stdlib (open=insert, read/write systems, `os.close` in the dtor), and then (3) reworking
`autodrop_in_ir` onto it — until which it stays red.
