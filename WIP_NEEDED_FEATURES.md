# WIP: language features the io-as-data conversion needs (TEMPORARY)

> **Scratch.** Concrete feature gaps surfaced while converting real code (web server, patterns docs) to the
> io-as-data / systems model. Each is a thing the current language can't express cleanly. Delete once these
> are designed + landed (or explicitly rejected). Companion to `WIP_DECISIONS.md`.

## 1. A way to get the handle of the CURRENT ROW inside an `each` (to consume/delete it)

**The gap.** The producer/consumer pattern only works if a consumer can *consume* a row — remove it so a
later pass doesn't re-process it. Today an `each (query { … })` body has no reference to the row it is
iterating, so it can't delete it:

- `delete(h)` needs a handle, and an `each` exposes no handle for its current row.
- `delete()` (bare) compiles (only a `discarded_ok` lint) but does NOT delete the iterated row — a consumer
  `each` over a pool prints its rows, and **running it again prints them all again** (verified: `do 1 / do 2`
  then `do 1 / do 2` again). So there is no real "consume."

**Why it matters.** Without it, "producer writes rows / consumer drains them" isn't expressible — the
consumer can only READ, never remove. Every real pipeline (a web server draining handled connections, a work
queue, any "process these then they're gone") needs the consumer to take the row OUT of the pool. The
`docs/patterns.md` producer/consumer example is currently wrong because of this (its consumers don't consume).

**What's needed (design TBD).** Some way for an `each (query { … })` (and `system(query)`) body to name the
current row's generation-checked handle, so it can `delete(<that>)(ok:)`. Candidate surfaces:
- bind it in the query header, e.g. `each (h: query { … })` or a reserved `#self` / `#handle`;
- a bare `delete` (or `consume`) statement inside an `each` that targets the current row (the natural
  reading — but it must actually remove the row, unlike today's no-op `delete()`).

Deleting the row being iterated also needs defined semantics (iterate a snapshot / free-list reuse) so a
drain in one pass is safe.
