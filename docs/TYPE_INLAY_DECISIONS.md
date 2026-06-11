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

## Follow-up: no "hint metadata" — complete the general map, plugin is a pure reader

Plan: `~/.claude/plans/structured-drifting-fern.md`. The principle, made load-bearing: there is **no
inlay-specific hint table**. Types live in the **one general node→type map** (`sem_model_expr_type_id`),
the same map every consumer reads. The editor derives the inlay by *comparing* the concrete syntax it
already has against that map — `O(1)` per binding, no per-kind logic. The compiler's only job is to make
the map **complete**.

- **The node→type map is the single source of truth and must be COMPLETE for every binding node.** A
  binding node having a recorded type is now an *invariant*, not a curated subset — so the per-kind
  computation is a real compiler fact (reusable by hover / go-to-type), never a side channel.
- **`decl_display_type_id` → `sem_decl_type_id`, exhaustive, no `default`.** The switch covers every
  `DeclKind` explicitly; a new kind is a `-Wswitch` compile error, never a silent `UNKNOWN` drop. Added
  arms: `DECL_ENUM` → the `type` meta (an enum *denotes a type*, like an alias); `DECL_STATIC` array →
  `tyid_of_array(static_type_id)` (**`static_type_id` is the ELEMENT type for arrays** — the array node
  must denote the array type, not the element; note a static array always carries a written type so its
  inlay is suppressed, but the *fact* in the map is now correct); `DECL_FUNC_GROUP` → UNKNOWN (an overload
  set has no single type — a principled "none"); `DECL_WORLD`/`DECL_USE` → UNKNOWN (not bindings).
- **Locals record their type UNCONDITIONALLY (`SN_BIND_STMT` handler).** Previously a local recorded its
  type only when *un*annotated, and recorded `type-of(RHS)`. Now: annotated → the **declared** type
  (`btype_id`), else → `type-of(RHS)`, always keyed on the bind node. This makes the map complete for
  locals too and removes the asymmetry with top-level. Suppression of an already-written type now lives in
  exactly ONE place: the presentation check `sv_type_count > 0` in `emit_type_hint`.
- **NO redundancy filtering for now — every binding's type is SHOWN.** Decision (deliberate, current
  state): the editor renders the type for *every* binding with an elided `⟨type⟩` slot, including the
  "super redundant" form types — `add : func(i32, i32) -> i32 : func(){…}`, `step : sys(pos, vel) : sys(…)`,
  `Particle : archetype : arche{…}`, `Color : type : enum{…}`. The only thing suppressed is a slot the
  source *already wrote* (`sv_type_count > 0` — there is nothing to infer there). The goal is to *see* all
  the redundant hints first; a thin "hide the redundant ones" layer comes **later**. `g_full_type_hints` /
  `--full` is retained as inert plumbing for that layer.
- **The redundancy is SYNTACTIC, not type-kind-based — proven by enum.** The earlier attempt used a
  type-based predicate `tyid_is_form_type` (func/proc/sys/policy/archetype-category). It cannot express
  enum: there is **no `TYK_ENUM`**, so an enum decl's type collapses to the *same* `type` meta as an
  alias — a type-based rule literally can't "hide enum, show alias." That forced a special-case-to-SHOW
  for enum, which is the wrong direction (show is the universal rule; hide is the special case). The
  predicate was removed. When the hide layer lands it should key on the **source form** — the binding's
  RHS is a definition-form node (`SN_FUNC_EXPR`/`SN_PROC_EXPR`/`SN_SYS_EXPR`/`SN_POLICY_EXPR`/`SN_ENUM_EXPR`,
  or the decl is `SN_ARCHETYPE_DECL`) — so enum falls out with zero special casing.
- **`decl_display_type_id` → `sem_decl_type_id`, exhaustive, no `default`.** The switch covers every
  `DeclKind` explicitly; a new kind is a `-Wswitch` compile error, never a silent `UNKNOWN` drop. Added
  arms: `DECL_ENUM` → the `type` meta; `DECL_STATIC` array → `tyid_of_array(static_type_id)`
  (**`static_type_id` is the ELEMENT type for arrays** — the array node must denote the array type, not the
  element; a static array always carries a written type so its inlay is suppressed, but the *fact* in the
  map is now correct); `DECL_FUNC_GROUP` → UNKNOWN (overload set — no single type); `DECL_WORLD`/`DECL_USE`
  → UNKNOWN (not bindings).
- **Locals record their type UNCONDITIONALLY (`SN_BIND_STMT` handler).** Previously a local recorded its
  type only when *un*annotated, and recorded `type-of(RHS)`. Now: annotated → the **declared** type
  (`btype_id`), else → `type-of(RHS)`, always keyed on the bind node — completing the map for locals too
  and removing the asymmetry with top-level.
- **Test fallout (deliberate, behavior change).** Every `explicit_view` test whose dump now contains form
  decls gained the redundant form-type inlays: `file_scope_type_hints`, `infer_var_type`, `alias_backing`,
  `param_hints` updated counts/lines; `full_type_hints` repurposed (forms now show by default; `--full` is
  a no-op); new `binding_type_completeness.arche` proves the full matrix + enum + forms all show.
- **Rejected:** a `canonical_type_id` field on `DeclSummary` (the decl type is single-call and the map is
  already the one store — a second store with no second reader); and literally merging the two recording
  paths (they consume different inputs at different stages — the unified thing is the *invariant + key*,
  not the code).
