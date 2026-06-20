# TEMPORARY — Phase 1 (system + #schedule + tick) autonomous decision log

> **This is a scratch document.** It records decisions made while autonomously implementing the
> approved plan (`~/.claude/plans/alright-lets-implement-it-piped-pony.md`). Delete once the work is
> reviewed and the durable notes are folded into the design docs / commit messages.

## Scope
Phase 1 of the flat effect model: the `system` composer kind, the `#schedule` region, the `tick()`
builtin, the driver loop. Deferred: `Eff`/`|>` (Phase 2), all restrictions + the export rule (Phase 3).

## Decisions (append as made)

- **(setup)** Baseline `make` is green before starting. Logging decisions here as I go; building after
  each stage because CFLAGS use full `-Werror`.

- **(window = device-private singleton)** Settled how a device exposes window state, a consequence of
  the export rule. The window *handle/connection* (X11 display ptr, fd) is a device-INTERNAL singleton:
  never named by user code, read only by gfx's own systems (`present`/`clear`), sitting behind the public
  systems exactly as the private procs/externs do. Window *properties* (width/height) surface only when
  user code reads them — gfx publishes a read-only `Screen { width, height }` singleton a layout map reads
  as ambient state. Rationale recorded: a system taking no params is the uniform state model (all state in
  pools, kernels+systems read ambient state in place), NOT a map-arg workaround; threading the window by
  value would be the one inconsistency. Folded into `docs/design/the-flat-effect-model.md` §device surface.

- **(qualify pass / @unknown fix)** Qualified calls (`fmt.printf`) inside a `system` body lowered to
  `@unknown`. Root cause: the qualify pass `hir_q_decl` (lower.c) rewrites `mod.name`→`mod_name` per
  decl body but had no `HIR_DECL_SYSTEM` case → system bodies fell through `default:`. Added the case
  mirroring HIR_DECL_PROC/MAP.

- **(`#schedule` node)** `#schedule { a; b; }` parses to a dedicated `SN_SCHEDULE_DECL` (not overloading
  `SN_REGION`), each entry a child `SN_NAME_EXPR`, `;`-separated (trailing `;` ok), block-form only,
  non-empty. Sits outside the `[SN_WORLD_DECL, SN_USE_DECL]` lowering range guard (like `SN_REGION`), so
  collected by an explicit root-only branch — which also enforces "schedule is driver-owned, collected
  only from the entry file, never from an inlined module".

- **(schedule entry validation)** New pass `sem_check_schedule` (runs after the decl table is built):
  every entry must resolve to a `DECL_SYSTEM` or `DECL_SYS` (map). Proc/func/query/unknown → E0063
  `schedule_entry_not_runnable`. A second `#schedule` → E0121 `duplicate_region` (reused).

- **(`tick()` = reserved builtin, not keyword)** Recognized in EXPR_CALL like `select`/`insert` (so it
  is never "undefined"); codegen lowers a 0-arg `tick()` to `call void @arche_tick()`. Guards: arity 0;
  E0065 `tick_in_map`; E0066 `tick_in_system`; E0064 `tick_no_schedule` (no `#schedule` declared). Driver
  (`proc`)-only by these guards. Statement-only classification deferred to Phase 3 (final restrictions).

- **(`@arche_tick` synthesis)** Codegen synthesizes `define void @arche_tick()` once, in the
  entry/whole-program unit, beside `@main`: per schedule entry, a `system` → `call void @<sym>()`, a
  `map` → reuse the `run <map>` pool-binding emission (synthetic HIR_STMT_RUN through codegen_statement).
  Diagnostic codes used: E0063–E0066 (scheduling family); `system` reuses no new code.

- **(invocation model confirmed)** `run <system>` is REJECTED ("unknown map") — a system is not a map and
  not callable as a statement. Systems are invoked ONLY by the schedule via `tick()`. This is the Phase 1
  design; the explicit system→system / scheduled-only ENFORCEMENT is Phase 3.

- **(slice verified)** `/tmp/sys_smoke.arche`: `map step` increments a `[1]Counter`, `system frame` runs
  step + prints, `#schedule { frame; }`, `main` loops `tick()` 3× → prints `frame v=1/2/3`. End-to-end
  green.

## Phase 1 COMPLETE

All four stages landed and verified:
- `system` kind, `#schedule` region, `tick()` builtin + `@arche_tick`, driver loop — end to end.
- Suite: **812/812 lit** green (803 prior + 9 new `tests/unit/language/schedule/`), doctests green,
  verify-fmt clean, C units green (semantic 33, codegen 9, lower 6, hotreload 7, inspect 9, syntax-view 28).
- New tests: 2 positive (system+schedule+tick in order; bare-map entry) + 7 negative (two-schedule E0121,
  proc/func/unknown entry E0063, tick-no-schedule E0064, tick-in-map E0065, tick-in-system E0066).

E0065 (tick_in_map) note: a bare `tick();` statement in a map is caught first by E0046 (map-not-a-
transform); E0065 fires for `tick()` in transform-RHS position (`v = tick()`), so the test exercises that
form. Not dead — distinct niche.

Deferred to later phases (unchanged from plan): Phase 2 = `Eff`/`|>`; Phase 3 = final restrictions
(system→system ban, tick statement-only/driver-only enforcement, systems scheduled-only, export rule,
derived ordering, startup phase, command-buffer/barrier).

## Audit pass — bugs found, concerns resolved (post-Phase-1 review)

### BUG fixed (ASan/UBSan caught it; functional suite missed it)
- **Uninitialized read of `ctx->schedule_node`.** `make_context` uses `malloc` + per-field init (not
  `calloc`), so `has_schedule`/`schedule_node`/`schedule_src` were garbage when no `#schedule` exists;
  `sem_check_schedule`'s `if (!ctx->schedule_node)` guard read poison (`0xbe…` under ASan) and could
  deref it. The 816-test functional suite masked it (glibc handed back zeroed pages). Fixed by
  initializing the three fields. **Regression guard:** `make test-asan` (semantic-test now has explicit
  `test_schedule_*` cases incl. an absent-#schedule case that exercises the previously-garbage path).

### Decisions ratified (from review questions)
- **`;` rejected in `#schedule`, mirror `#import`.** Entries are whitespace-separated (`#schedule { a b
  c }`); a `;` → "expected a system/map name", exactly as an import block rejects punctuation. Removed the
  optional-`;` consumption in the parser.
- **`tick` reserved (E0068).** A callable decl (proc/func/map/system) named `tick` is rejected — it would
  be silently shadowed by the builtin at every call site. (Answer to "why is it not [reserved]?": it just
  wasn't; now it is.) Renamed two pre-existing test maps that happened to use the name `tick`
  (query/subset_columns → `decay`, devices/.../privshape → `advance`).
- **`tick()` is statement-only (E0067).** A pure action with no value: legal only as a bare `tick();`
  statement, never `x := tick()` or nested. (Answer to "if tick is a proc why is it used like a VALUE?":
  it shouldn't be — now enforced via a `bare_expr_stmt` flag set only at SN_EXPR_STMT.) Reuses the
  effect-model principle that actions aren't values.
- **Systems are global like types/pools.** Recorded as the design principle (doc §schedule): a system has
  one bare name in the global vocabulary, scheduled `draw` not `gfx.draw`. The cross-module/device
  *implementation* (global system registration + cross-unit `@arche_tick` declares) remains Phase 3; the
  Phase-1 parser accepts bare local names only. This reframes the earlier "qualified schedule entries"
  concern: the answer is global bare names, not qualification.

### New diagnostics: E0063 schedule_entry_not_runnable, E0064 tick_no_schedule, E0065 tick_in_map,
### E0066 tick_in_system, E0067 tick_not_statement, E0068 tick_reserved_name.

### Final state: 816/816 lit (4 new + earlier 9 schedule tests), semantic-test 36/36 (3 new), ASan
### 36/36 + 6/6 + 7/7 clean (no UB, no leaks), verify-fmt clean.
