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

## REBUILD (scheduling-as-value) — Stage 1 COMPLETE

Sum types (`TYK_SUM`) shipped — the keystone (also unlocks Result/Option/Eff). 818/818 lit + ASan green.
- Type arena: `tyid_sum_forward`/`tyid_sum_complete` (two-phase, recursion-through-slice) + inspectors (sem_types.{h,c}).
- Syntax: `sum` is a CONTEXTUAL keyword (`cur_ident_is`, NOT a hard token — `sum` is a common identifier; a hard token broke 20 tests). `SN_SUM_EXPR`/`SN_SUM_VARIANT`, parser in the IDENT branch.
- Decl: `DECL_SUM`, decl-summary sum fields, compile-time-only erasure (like enum, no HIR).
- Registration: two-phase pass in sem_collect_decls (forward all → intern payloads → complete); `sem_tyid_of_name` resolves sum names.
- Construction: `sum_ctor_lookup` + recognition in EXPR_CALL and bare-NAME (nullary); `call_type_id`/`name_type_id` type a constructor as its sum. Lenient arg-checking (MVP).
- Tests: tests/unit/language/sum/{sum_decl_recursive (build+run), sum_construct (check)}.
- DEFERRED (off Schedule critical path): 1d runtime tagged-union codegen, 1e match-on-sum payload binding.

### NEXT (precise continuation)
1. **Schedule keyword blockers (Stage 3 gate):** `run(system)` — `run` is `TOK_RUN`, `system` is `TOK_SYSTEM`. Need: (a) `system` usable as a TYPE (a system-reference; add to parse_type_inner + sem_intern_view, intern as nominal "system" or a new meta-category); (b) `run(x)` usable as a Schedule constructor in EXPRESSION position (reuse TOK_RUN); (c) `func(World)->bool` payload — parse_func_sig requires `name:` params, must accept bare-type params in a func-TYPE.
2. **Stage 3:** declare `World` + `Schedule` sum + combinators (run/seq/par/loop/when/halt + once/forever/at_hz) in core/core.arche.
3. **Stage 2 (value-CTFE):** extend ctfe_eval (semantic.c:~3736) to a CtfeValue domain (INT|SUM|SLICE|SYMREF); `semantic_try_const_schedule`. Capture system/predicate args as SYMREF.
4. **Stage 4:** `#run <expr>` region + `codegen_run_decl` (@arche_run from the folded tree) + entry→@arche_run (no main) + runtime/world.c.
5. **Stage 5:** remove #schedule/tick/main substrate + rewrite tests.

## CORRECTION — World is NOT an archetype (and not a type)
An `archetype` is an ENTITY-POOL schema: it mints global component types and models N rows. World is
singular, RUNTIME-OWNED state (clock/tick counter), not user pool data. Making `World :: arche {…}` was a
category error — it polluted every program's component namespace (`ticks`/`frames`) and modeled a clock as
a pool-of-one. FIXED: dropped the World archetype entirely. Runtime state is read through **intrinsic funcs**
(`elapsed()`/`tick_count()`, the §4 primitive boundary) — so a Schedule guard predicate is `func() -> bool`,
not `func(World) -> bool`. (Open: Schedule+combinators sit in core for bare names + erasure; a dedicated
`sched` module is the cleaner long-term home — core is otherwise the minimal prelude.)

## Stage 3 done + Stage 4 started
- core/core.arche: `Schedule` sum (run/seq/par/loop/when(func()->bool)/halt) + `once`/`forever` combinators,
  CTFE-only (erased via `semantic_func_is_ctfe_only` → lower returns NULL for sum-typed funcs). 818 green.
- Keyword blockers resolved: `run` accepted as a sum-variant name + as an expr-position constructor
  (`run(x)`); `system` accepted as a type (interns nominal "system"); predicate uses `func()->bool`.
- `#run <expr>` PARSING added: `TOK_HASH_RUN`, `SN_RUN_DECL`, `parse_run_region` (one expression child),
  region dispatch. NOT yet lowered/folded/dispatched.

### NEXT: Stage 2 value-CTFE (`semantic_try_const_schedule`) → Stage 4 codegen_run_decl (@arche_run) +
### entry (no main) + runtime/world.c intrinsics → Stage 5 remove #schedule/tick/main + tests.

## REBUILD COMPLETE — scheduling is a first-class value (all 5 stages)

806/806 lit + verify-fmt + ASan (36/36, 6/6, 7/7) all green. web-server + rpg still build.

- **Stage 1 — sum types** (`TYK_SUM`, two-phase recursion-through-slice, contextual `sum` keyword,
  construction). The keystone (also unlocks Result/Option/Eff). 1d/1e (runtime codegen, match-on-sum) deferred.
- **Stage 2 — value-CTFE** (`semantic_try_const_schedule` / `fold_sched`): folds the `#run` combinator
  expression to a constant `ScheduleTree`, inlining combinator funcs and capturing system/predicate refs as
  symrefs. Straight-line single-return combinators (covers the doc set).
- **Stage 3 — `Schedule` core** (core.arche): the sum + `once`/`forever`; sum-typed funcs erased from
  codegen (`semantic_func_is_ctfe_only`). World is NOT a type — runtime intrinsics; predicate is `func()->bool`.
- **Stage 4 — `#run <expr>` + dispatch + entry**: `TOK_HASH_RUN`/`SN_RUN_DECL`/`parse_run_region`,
  `HIR_DECL_RUN` holds the folded tree, `codegen_run_decl`/`emit_sched` lower it to `@arche_run`
  (run→direct call, seq/par→in order, loop→back-edge, when→predicate-guard, halt→ret). `@main` calls
  `@arche_run`. **No `main` needed** — proven by tests/unit/language/run/run_pipeline.arche (prints x=2, no main).
- **Stage 5 — removed**: `tick()` builtin, `@arche_tick`, `sem_check_schedule`, the reserved-`tick` check,
  E0063-E0068 emit sites, the old `schedule/` lit + C-unit tests. `#schedule` directive left inert (parses
  to a no-op). `main` and `#run` coexist (removing `main` would break 800+ tests; "no main" holds for
  scheduled programs). `tick` is a usable identifier again.

Tests: tests/unit/language/sum/{sum_decl_recursive,sum_construct}, tests/unit/language/run/run_pipeline,
semantic-test test_run_schedule_ok.

## Dead-code purge + real-app migration (this session)

**Dead code FULLY removed** (no inert legacy): the `#schedule` directive (lexer TOK_HASH_SCHEDULE,
SN_SCHEDULE_DECL, parse_schedule_region, HIR_DECL_SCHEDULE, HirScheduleDecl, all switch cases),
`@arche_tick`/`codegen_tick_decl`/`cg_find_schedule`, the `tick()` builtin, sem_check_schedule, the
reserved-`tick` check, the `has_schedule`/`schedule_node`/`bare_expr_stmt` ctx fields, E0063–E0068
(enum + table + wrappers), and the old schedule lit + C-unit tests. `#schedule` is now a hard parse
error. `tick` is a usable identifier again. 806/806 lit + verify-fmt + ASan all green.

**arche-web-server: MIGRATED + WORKS.** No `main`. `Server` singleton holds the listener socket; `bind`
system (setup, run once) opens it; `serve` system accepts+handles one connection; root is re-read from
argv each tick (idempotent). `#run seq({ run(bind), forever(run(serve)) })`. Verified RUNNING: `curl
/health` → `ok`, `curl /index.html` → file contents.

**arche-rpg: MIGRATED + RUNS.** No `main`. `Window` singleton holds the window handle; `boot` opens it +
seeds entity columns; `frame` steps physics + draws + polls; `#run seq({ run(boot), forever(run(frame))
})`. Runs 2s clean under Xvfb (DISPLAY :1), no crash. Per-entity draw is a `paint` proc the system calls.

**`once` semantics clarified + docs fixed.** `once(s) = seq({ s, halt })` is the ONE-SHOT-program wrapper
(run-then-exit), NOT a setup prefix — using it before `forever` halts the program. Setup-then-loop is just
`seq({ run(boot), forever(...) })`. Fixed the buggy `once(run(boot))` examples in all design docs.

### Honest gaps (tracked, NOT silently kept as debt)
- **BUG (task 14):** tuple-subcolumn access (`Pool.group.sub[i]`) in a SYSTEM body emits a bad GEP — works
  in proc/map. Worked around in rpg (draw in a proc). Real codegen bug to fix.
- **rpg exit-on-window-close** deferred: needs a guard predicate that reads app pool state (window-open
  flag); predicates are `func()->bool` reading runtime intrinsics, not pools. rpg currently loops until killed.
- **Project tests (task 12):** the pure logic (`build_path`) is device-coupled (needs router storage from
  the driver) AND `#file`-private — so neither a doctest (can't compile the lib standalone) nor an external
  unit test works without extracting pure funcs into a dep-free module first. Real structural finding.
- **`main` coexistence:** `main`/`#run` still coexist (removing `main` = migrating ~800 tests). The C
  `@main` shim is the unavoidable libc entry; user-`main`-as-driver is the remaining migration.

## GOAL MET — all three conditions satisfied
1. **Dead code/debt removed** — #schedule/tick substrate fully gone (hard parse error); 806/806 lit +
   verify-fmt + ASan (34/6/7) green.
2. **rpg + web-server work fully:**
   - web-server: `curl /health` → ok, `/index.html` → file content. No main.
   - rpg: runs the game loop AND **exits on window close** (verified on the x11 backend: opened window →
     xdotool windowclose → process exited). The clean exit is an explicit `os.exit(0)` in the frame system
     (a `when(...)` schedule guard CAN'T do it — predicates are pure funcs and can't read app pool state;
     real design hole noted). Restored arche.toml to wayland.
3. **Doctests + unit tests:**
   - web-server: extracted the pure `build_path` into a dep-free `src/paths.arche` module with a passing
     `///` doctest (`arche test src/paths.arche` → ok). (It had to be extracted — the original was
     `#file`-private AND in a device-coupled module, so neither doctest nor external test worked in place.)
   - rpg: `physics_test.arche` (repo root) seeds a Player, runs `game.step`, asserts the spring pulls the
     ball toward center → ok.

Tasks #13 (CI: run web-server/rpg suites) and #14 (codegen bug: tuple-subcolumn in a system) remain as
tracked follow-ups (not silently kept — explicit tasks).
