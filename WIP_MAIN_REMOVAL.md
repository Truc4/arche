# WIP: remove `main` as an entry point (TEMPORARY)

> Scratch. Delete when `main` is no longer special and `#run` is the only entry.

## Goal
Convert every `main :: proc()` → `#run`, then remove all `main`-specific compiler code so a decl named
`main` is treated identically to any other name. Add a test proving it.

## Done
- Codemod (573 files, scripted/recoverable): `main :: proc() { body }` → `entry :: system { body }` + `#run
  entry`. (`main` collides with the emitted C `@main`, so the system is `entry`.) A `system` may call a
  helper `proc` during the transition (verified). Skipped: 11 arg-mains, 56 `lints/errors/`, 2 already-`#run`.
- Fixed a real soundness hole: `tycheck.c` only type-checked `func`/`proc` bodies — `system`/`each`/`map`
  bodies were unchecked (bad binds/args/`break`/type mismatches silently compiled). Now it walks all of them.
  State: 856/866.

## Decision applied: bare `name :: int` is DISTINCT (user). `alias` keyword = transparent.
So tycheck is right to flag lax sites. Resolved the 10 failures → **866/866 green**:
- `gather`: `out = out(Table.val[v])` (explicit conversion, distinct-correct).
- `opaque_into_column`: tycheck false-positive fixed — the opaque create-once seal now exempts a column
  init/scatter (`Slot.slot = { h }` is a drop-managed MOVE into a row, not a local overwrite).
- `implicit_move_no_double_drop`: IR-symbol test updated `@main_user` → `@entry`.
- 7 analyzer-`--dump` tests (`explicit_view/*`, `lint_unused_query`): REVERTED to `main`. They assert on the
  dump of `main :: proc()` itself (`type proc()()`) — they test the analyzer's view of a proc, not the entry
  mechanism. Once `main` is just a normal name, `main :: proc()` is still a valid proc that dumps the same,
  so they keep passing (no conversion needed). These are part of the broader proc→system test sweep, not
  main-removal.

## Still to convert before the mechanism can go
- 11 arg-mains (`main(argc)`) → `entry :: system` reading `os.argc()`.
- the build-AND-run subset of the 56 `lints/errors/` mains (negative/expect-error ones need no entry).
- doc `main`s.

## DONE — `main` is no longer the entry (all green: 871 lit+doctests + corpus + test-install)
- codegen: removed the C-`@main` → `@main_user` auto-call; `@main` now only dispatches `@arche_run` (the
  `#run` schedule). `cg_fnsym` renames ANY decl named `main` → `main_user` (mechanical C-`@main` collision
  avoidance, not a special role) — so `main :: system { … }` + `#run main` works like any name.
- `#run` is the ONLY entry. An unscheduled `main` does not run.
- Doctest harness (doctest_run.c, both the core/in-file path AND the .md shared-context path) wraps loose
  statements in `entry :: system { … } #run entry`, not `main :: proc()`.
- Converted the stragglers the first codemod missed (aligned `main   :: proc()`), the 5 `examples/`, 3
  `extras/` demos; fixed `scripts/check_corpus.sh` + the `test-install` Makefile target to use `#run`.
- Removed the `main` lint exemption (semantic.c ~4620).
- Test: `tests/unit/language/run/main_is_not_special.arche` — an unscheduled `main` never runs; the entry
  is whatever `#run` names.

## RESIDUAL main-specific code (coupled to 7 dump tests)
Two sites still name `main` (semantic.c): `dead_is_root` (~8590: `main` is a dead-code reachability root)
and the closed-world "binary" detection (~8680: keys off a `main` DECL_ORIGIN_ENTRY; should key off `#run`).
The 7 reverted analyzer-`--dump` tests (`explicit_view/*`, `lint_unused_query`) DEPEND on `main`-as-root —
they assert reachability/dead-code counts from a `main` program. To remove these two sites: convert those 7
tests to a `#run`-scheduled entry (which is already a root per the `map/system/each are entry points`
comment) and reconcile their expected dumps. The ~41 `lints/errors/` mains are fine as-is (they expect
compile errors, never run `main`).
