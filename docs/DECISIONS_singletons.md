# Autonomous decision log — ECS singletons for systems

Context: implement "a system reads/writes a shared singleton" (gravity/dt/input/config), per the approved
plan (docs/../.claude plan "Singletons for systems"). Working autonomously toward: fully implemented,
green, odin/jai-reviewed, concerns addressed.

## D1 — The approved plan's core assumption was FALSE (verified empirically)
The plan claimed a `sys` body can already read/write a pool directly (`Config.gravity[0]`) whole-program,
so only a hot-boundary fix was needed. **Disproven by a quick test:** `Config.gravity[0]` inside a sys
body miscompiles even whole-program. The explore that produced the claim conflated "an archetype NAME
resolves to its global" (true) with "an explicit indexed access works in a sys body" (false).

Root cause (codegen.c:2800 and :2865): a field access `Config.gravity` inside a sys body auto-indexes by
the implicit row-loop counter (`implicit_loop_index`) — designed for the ITERATED archetype's own columns —
even for a FOREIGN archetype (`Config`) unrelated to the iteration. Result: `Config.gravity` → loads
`gravity[loop_index]` (wrong + OOB for a `[1]` pool), and the explicit `[0]` then indexes the loaded i32 as
a pointer → IR type error (`'%vN' defined with type 'i32' but expected 'ptr'`).

## D2 — Fix: an explicit index suppresses the sys row-loop auto-index for its base
In HIR_EXPR_INDEX codegen, evaluate the index BASE with `implicit_loop_index` (and `vector_lanes`)
temporarily cleared, so the base yields the column/array POINTER and the EXPLICIT index applies. Rationale:
writing `[i]` means "this index," never the enclosing loop's. This fixes `Config.gravity[0]` (singleton
row 0), is correct for any explicit index in a sys (`v[0]` etc.), and is local to the index path. Verified
against the full suite for regressions.

## D3 — Scope grew from "1 hot fix" to "1 codegen fix (D2) + 1 hot-boundary fix"
So the feature is: (D2) make explicit singleton access compile correctly everywhere, AND the originally
planned hot-boundary fix (device units extern non-owned pool globals so a hot `.so` binds the host's
singleton, not its own copy). No new syntax; "singleton" stays the convention "a `[1]` pool read in the
body."

(further decisions appended below as work proceeds)

## D4 — Hot-boundary fix NOT needed; weak-symbol interposition already shares the host pool
Tested empirically: in `arche run` (hot), a device `.so`'s system that references `@Config` reads AND
writes the HOST's Config, not its own. Mechanism: `linkonce_odr` lowers to a WEAK symbol; the host is
linked `-rdynamic` (exports its globals); at dlopen the host's `@Config` interposes the device's weak copy,
so they share one allocation. Verified both directions: device reads host's value (v0=10 v1=110 g=10), and
device writes are seen by the host (write-through tick matched whole-program). So the planned
"extern-in-device / define-in-owner" change is DROPPED — unnecessary. (Entity pools are still passed as
params, but for POLYMORPHISM/dynamic count, not for sharing.) Works in all three modes: whole-program (one
@Config), per-unit non-hot (linker folds linkonce_odr), hot (weak interposition).

## D5 — A sys statement that writes ONLY a singleton runs once, not per-row (documented limitation)
`bump :: sys (v) { Counter.tick[0] = Counter.tick[0] + 1 }` over N entities yields tick=1, not N. arche
systems have no unified per-row body loop — each assignment to an ITERATED column generates its own row
loop; a statement touching only a foreign singleton isn't looped, so it executes once. Consequence:
per-entity ACCUMULATION into a singleton (sum/count) is not expressible (needs the same unified-body-loop
that per-row branching needs — both OUT of scope). The SUPPORTED, useful pattern is READING shared context
(`v = v + Config.gravity[0]`), which loops correctly because the assignment target (`v`) is an iterated
column. The driver sets singletons; systems read them. This matches the feature's goal.

## D6 — Singleton idiom requires one `insert` to create the instance
A `[1]Config` pool has capacity 1 but 0 LIVE rows until `insert`. Reading `Config.gravity[0]` bounds-checks
against the live count, so the driver must `insert(Config, …)` once to create the singleton (like flecs
`ecs_singleton_set`). After that, `Config.gravity[0]` reads/writes row 0 freely. Direct
`Config.gravity[0] = v` WITHOUT a prior insert aborts (count 0). Documented as the idiom.

## Net feature = D2 only (one codegen fix) + the insert idiom. No new syntax, no hot-boundary change.

## D7 — odin/jai senior review: concerns triaged + addressed
Both reviewers: "ship-with-fixes." Resolutions:
- **D5 footgun (their #1: a sys writing only a singleton runs once → silent wrong accumulation).** FIXED with
  a hard error **E0215 (sys_writes_foreign_pool)**: a system may READ a foreign singleton but not WRITE one
  (the leftmost target IDENT resolves to an archetype != the system's iterated shape; find_archetype
  canonicalizes aliases so it's alias-safe). Turns the silent run-once trap into a clear compile error
  ("a system READS shared singletons, the driver WRITES them"). Negative test:
  tests/unit/language/systems/sys_write_foreign_pool.arche. No false positives across 723 tests.
- **C1 weak-symbol interposition robustness.** KEPT (verified working both directions; the read path is
  tested whole-program + hot). Hardening done: this doc + a contract note; arche does NOT pass
  -fvisibility=hidden and the reload runtime uses RTLD_NOW|RTLD_LOCAL (NOT RTLD_DEEPBIND, which would
  invert lookup and break sharing — must stay that way). DEFERRED with rationale: explicit pointer-passing
  is the bulletproof fallback (every shipping engine does it) and remains the escape hatch if the weak path
  ever bites; not done now because it reopens the ABI threading the design deliberately avoided and the
  mechanism is standard ELF + tested. (Note: foreign WRITES are now E0215, so the only shared access is a
  READ — strictly simpler/safer for interposition.)
- **C2 write-through hot test.** MOOT: foreign writes from a system are now E0215, so there is no
  device→host shared write to test. The device→host shared READ in hot is covered by singleton_read.arche's
  `arche run` RUN line.
- **D2 lock (N1/S1).** singleton_read.arche IS the lock: `Config.gravity[0]` reading row 0 (the explicit
  index) only works because D2 honors the literal over the loop index; a regression would crash/misread.
  Suite green confirms no per-row indexed-access regression.
- **S2 signature read/write-set marker (for future auto-parallelism).** DEFERRED with rationale: arche does
  not schedule systems in parallel yet; a marker is premature. Recorded as the prerequisite to add BEFORE
  any parallel scheduling (an unmarked singleton read is a hidden data dependency a scheduler can't see).
- **N2 idiom `insert(Config,…)(_:, _:)`** uses the out-arg colon form; harmless here (`_`), noted.

## D8 — arche-rpg demo uses the singleton (end-to-end proof)
arche-rpg's physics tuning (stiffness, center_x, center_y) moved from `::` consts into a driver-owned
`[1]Config` the driver sets via `insert(Config, 24, 320, 240)`; `game.step` reads `Config.stiffness[0]` /
`Config.center_x[0]` / `Config.center_y[0]` as shared context. Verified: release builds + headless renders
+ release IR pure (0 arche_hot_resolve); the spring is numerically correct (x: 80→90→109→…→346 toward 320)
and HOT mode yields identical values — the device `.so` reads the HOST's Config singleton live (weak
interposition), and `arche run` hot-reloads game.arche edits with the singleton intact.

## Final state
Feature SHIPPED: a system reads a shared singleton (a `[1]` pool) directly in the body; works whole-program
and hot (no reload indirection — direct global, shared via -rdynamic weak interposition). One codegen fix
(D2: explicit index suppresses the sys row-loop auto-index for its base) + one guard (D5/E0215: a system
may read but not write a foreign pool). No new syntax. Tests: per_unit/singleton_read.arche (read,
whole-program + hot + purity), systems/sys_write_foreign_pool.arche (E0215). 724/724 default + per-unit,
ASan clean, format clean.
