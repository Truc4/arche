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

- **The resource is a column TYPE, not a wrapper archetype.** There is no `File` shape to wrap an fd in. You
  park an `fd` column in *whatever* archetype you already have (`Conn { fd, addr }`, `LogFile { fd, path }`),
  and the fd is data in that row. "Open" inserts the row; the row exists or it doesn't; there is no descriptor
  to "close" as a value. (An earlier sketch proposed a dedicated `File` archetype — dropped: it's a wrapper,
  and arche queries by type, not by named container.)
- **Operations apply to rows, via queues.** To read, insert a request row (a `ReadReq` pool referencing the
  `File` by entity handle); a `read` system drains the queue, performs the syscall, and writes results back
  (a `ReadResult` pool/column). Same command-buffer pattern as `insert`/`delete` and event pools — io is
  systems-over-pools, on the flat-effect spine (effects in systems, data in pools).
- **Synchronous by ordering, not by blocking.** `seq({ enqueue_read, drain_read, consume })` is synchronous
  within one pass: by the time `consume` runs, the read already happened in `drain`. And the queues create
  the edges automatically — `enqueue` writes `ReadReq`, `drain` reads it → drain-after-enqueue for free;
  `drain` writes `ReadResult`, `consume` reads it → consume-after-drain for free. No async machinery; no
  mmap-vs-stream split (both are just systems the schedule sequences).
- **close = the row leaving the pool.** Deleting a row with an `fd` column fires the kernel close — a
  `@drop(fd)`-on-delete hook (registered once in stdlib io), the analogue of RAII but tied to the row's
  lifecycle, not lexical scope. The hook is **column-type-targeted**: on any row delete, each column whose
  *type* has a registered `@drop` is released. (`autodrop_in_ir`'s scope-exit premise is *replaced* by this:
  delete the row → close emitted.)
- **Passing a handle stays allowed** as an escape hatch, but it buys nothing — operating on a row is strictly
  easier than threading a handle — so there is no hard datasheet seal forcing it; the pool is the natural
  model.

## What this unblocks

`autodrop_in_ir` is **reworked and green** under this model: open an fd, park it in a pool row, delete the
row → `@drop(fd)` (registered in stdlib io) emits `os.close` in the IR. The old scope-exit-RAII-on-an-`fd`-enum
premise is gone.

## Status / scope

Implementation is phased and separate: (1) data-derived ordering as the default + `depends on`;
(2) io-as-data (close-on-delete, then request/result read/write pools); (3) `autodrop_in_ir`. The `fd` enum
remains for the three standard streams (stdin/stdout/stderr), which are never opened or closed.

**Landed:** the **column-type-targeted `@drop`-on-delete** hook + the io close path. `@drop(T)` registers a
destructor keyed by the type `T` (opaque or enum); it fires whenever a value of type `T` dies — an opaque
local at scope exit (RAII, pre-existing) or a pool **column** of type `T` when its row is `delete`d. On row
delete, `@arche_delete_<arch>` walks the dying row's columns and calls the dtor of each whose *type* has one
registered, loading the column value at the validated slot. stdlib io registers `@drop(fd)` → `os.close`, so
parking an fd in any pool and deleting the row closes it (`tests/unit/language/drop/autodrop_in_ir.arche`).
**Still open in phase (2):** request/result read/write pools (io operations as systems over queues).
