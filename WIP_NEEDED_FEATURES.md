# WIP: language features the io-as-data conversion needs (TEMPORARY)

> **Scratch.** Concrete feature gaps surfaced while converting real code (web server, patterns docs) to the
> io-as-data / systems model. Each is a thing the current language can't express cleanly. Delete once these
> are designed + landed (or explicitly rejected). Companion to `WIP_DECISIONS.md`.

## 1. ✅ DONE — current-row handle in an `each` (to consume/delete it): `each (query {…} as w)`

**Landed (all green: 869 lit + doctests + corpus + fmt).** Decided from the grammar (sigils are for compiler
kinds `#`/`@`, never values; a row handle is a first-class `handle(driver)` value, named bare like `insert`'s
`h`) — so **no `$` sigil**: `each (query { … } as w) { … delete(w)(ok:); }`. `w` is a `handle(driver)` local;
`delete(w)` consumes (removes) the current row. Two parts:
- **Liveness fix** (the real bug): the `each` fan iterated `0..count`, but `delete` frees a slot WITHOUT
  shrinking count, so dead holes were re-walked (a delete-then-each re-processed removed rows; a drain never
  emptied). Fixed by a sign-bit liveness flag in `gen_counters` — `delete` sets bit 31 (gen < 0 = dead),
  `insert` clears it, the fan skips negative-gen slots. No new struct field; handle validation unchanged
  (a dead slot's gen never matches a live handle → aborts as before). Regression: `each_skips_deleted_rows`.
- **`as w` binding**: new `SN_QUERY_BIND` node; contextual `as` keyword in the each header (parser);
  `HirEachDecl.row_var`; semantic binds `w` as `tyid_of_handle(driver)`; codegen mints the row handle
  (`slot | gen<<32`) in the fan and binds it. Regression: `each_as_bind_consume`; `docs/patterns.md`
  producer/consumer now really drains.

**Future (this is the foundation):** the same `as w` is the doc's `$w` query variable — relationships add a
`where component == w` filter on a nested fan (`queries-systems-and-joins.md:202-220`) + an entity-reference
component + the join solver. The binding half is now built.
