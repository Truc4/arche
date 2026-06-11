# Type-inlay consistency — decisions log

Goal: the inferred-type inlay fills the elided `⟨type⟩` slot of the unified grammar
(`name : ⟨type⟩ (: | =) value`) for **every** binding form — `:=`/`::` always, no picking and
choosing. The inlay is a pure projection of `type-of(RHS)`; if a form has no compiler type, that's a
gap to close, not a binding to skip. Plan: `~/.claude/plans/1-lets-plan-this-parsed-wigderson.md`.

## Architecture

- The LSP reads one per-node channel: `sem_model_expr_type_id(model, sv_id(node))`.
- **Uniform key:** every binding's type is recorded keyed by the **binding/decl node id** (not the
  value-expr node), so locals and top-level decls share one convention and the analyzer needs no
  per-kind logic.
- **Analyzer:** one `emit_type_hint(node)` — skip if the `⟨type⟩` slot is written
  (`sv_type_count > 0`), else render `sem_model_expr_type_id(node)` at the `::`/`:=` anchor.

## Decisions (autonomous)

- **Uniform model key = binding/decl node id.** Locals record their RHS type onto the `SN_BIND_STMT`
  node (semantic.c, SN_BIND_STMT handler); top-level decls via one `sem_record_binding_types` pass
  keyed by `d->node`. The analyzer reads `sem_model_expr_type_id(sv_id(node))` for any binding — no
  value-node hunting, no per-kind logic. (Value bindings landed; 593 green.)
- **func/proc/sys are distinct type KINDS — the `is_proc` flag is removed.** The arena conflated them
  as one `TYK_FUNC` + `is_proc` bool; Arche's whole identity is that func≠proc≠sys, so they become
  `TYK_FUNC` / `TYK_PROC` / `TYK_SYS`. Structural inequality (`proc()(int) != func()->int`) now falls
  out of the kind, not a flag. "Any callable" sites (archetype-field ban, named callable alias) use a
  new `tyid_is_callable` helper; kind-specific sites pick the builder. HIR's own `is_proc` (hir.h:51)
  is an independent representation and is left untouched.
- **Display = Arche's own spelling:** `func(i32, i32) -> i32`, `proc(in)(out)`, `sys(comps)`. Not
  param/return counts (the old `func(2)->(1)` was a placeholder; cf. Odin/Jai show the real types).
- **`policy` is a distinct kind too.** Same reasoning as func/proc/sys — `clamp :: policy(len, i)` is
  its own form. Kinds are `TYK_FUNC` / `TYK_PROC` / `TYK_SYS` / `TYK_POLICY`. A policy is a `DECL_FUNC`
  with `is_policy`, so `decl_display_type_id` branches on `is_policy` to pick `tyid_of_policy`. Display
  `policy(i32, i32)`.
- **sys component params resolved by name.** A sys's params are bare COMPONENT names (`sys(pos, vel)`),
  not typed params, so `params[i].type_id` is unset — resolve each via `sem_tyid_of_name(name)` so it
  renders `sys(pos, vel)` not `sys(<unknown>)`.
- **Hide redundant by default; full hints opt-in (HIDE ≠ IGNORE).** The compiler records a type for
  EVERY binding (it is *there*). The analyzer renders only the non-redundant ones by default. Crucially,
  redundancy is decided by the **TYPE kind**, not the node kind — the parser makes `add :: func(){}` an
  `SN_CONST_DECL` (value = an `SN_FUNC_EXPR`), so node-kind can't tell value from form. `type_is_form`
  treats `TYK_FUNC/PROC/SYS/POLICY/ARCHETYPE_CATEGORY` as redundant (the form spells it) and hides them
  unless full mode; value types always show. This is why the existing `explicit_view` tests keep their
  ORIGINAL expectations (my mid-stream edits to `infer_var_type`/`alias_backing` were reverted).
- **Full mode is an editor-driven REQUEST, not a flag of the compiler.** `arche build`/compilation is
  untouched. The analyzer line protocol gains `HINTS full <path>` (vs plain `HINTS <path>`); the
  one-shot path gains `--dump --full`. The nvim plugin maps the user's inlay-verbosity setting to which
  request it sends. Tests: `file_scope_type_hints.arche` (default: value hints show, form hints hidden),
  `full_type_hints.arche` (`--dump --full`: func/proc/sys/arche render in Arche's spelling).

## Result

Implemented and green: 595 lit + doctests, ASan 6/6, semantic/codegen/lower unit bins. The type system
no longer conflates the forms (no `is_proc`); the inlay is one rule over `type-of(RHS)`; value bindings
are consistent at every scope; form types are recorded but hidden by default with an editor opt-in.
