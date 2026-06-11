# Plan: "see `:`, return type" — every binding's type is a complete compiler fact

## Context

The rule is one line: a binding is `name : ⟨type⟩ (: | =) value`; **see the `:`, return the type.** The
_mechanics_ (computing that type) belong to the **compiler**, must exist for **every** binding (no
kind silently dropped), and the editor side must be a dumb reader — "I'm not going to maintain LSP
stuff." Per-type rules that _hide_ messy/redundant inlays come _afterward_, as a thin layer; they may
never be the reason a type is missing underneath.

What's actually true today (verified): the type computation already lives in the compiler —
`decl_display_type_id` (semantic.c) → recorded into the node-id `SemModel` by `sem_record_binding_types`
(a normal semantic pass) → the analyzer just reads it. So "mechanics in the compiler" is already the
shape. The bug is that the switch has a **silent `default → UNKNOWN`**, so `enum`, `func_group`,
static-`array`, and (until just now) type-`alias` decls fall through → no type recorded → the inlay
shows nothing. That silent drop is exactly the "skipping" / "adding one kind at a time" — not a
universal system.

## Layer 1 — the compiler always has every binding's type (mandatory, complete)

`semantic/semantic.c` — make the per-decl type a canonical, **exhaustive** compiler fact:

- Rename `decl_display_type_id` → **`sem_decl_type_id(ctx, d)`** — "the `⟨type⟩` of declaration `d`".
  Make the `switch (d->kind)` cover **every** `DeclKind` explicitly, with **no `default`** (so adding a
  future kind is a compile-time forcing function, not a silent miss):
  - `DECL_CONST`: value const → its value type; type alias (`is_type_alias`) → the `type` meta
    (`sem_tyid_of_name(ctx, "type")`). _(alias arm already added.)_
  - `DECL_ENUM` → `type` meta (`Method : type : enum{…}`). **(gap — add)**
  - `DECL_STATIC` → scalar: `static_type_id`; array: its array `TypeId`; archetype-allocation
    (`Particle[4]`) is not a `name::value` binding → UNKNOWN. **(array arm — add)**
  - `DECL_FUNC`(incl `is_policy`)/`DECL_PROC`/`DECL_SYS` → the `tyid_of_func/proc/sys/policy` over the
    already-final signature (params/out_params/return_type_ids; sys components by name). _(done)_
  - `DECL_ARCHETYPE` → `tyid_of_archetype_category`. _(done)_
  - `DECL_FUNC_GROUP` → explicit arm: an overload SET has no single type → UNKNOWN **with a comment**
    (a principled "no single type", not a forgotten kind). **(add explicit arm)**
  - `DECL_WORLD`, `DECL_USE` → explicit arms: not bindings → UNKNOWN.
    This is the underlying mechanic: see a `::`/`:=`, the compiler returns the type — for everything, by
    construction. Signature-only typing reads `sem_fill_decl_type_ids`' already-final TypeIds, so it never
    re-walks a body (no double diagnostics). Recorded for every binding node in `SemModel` (top-level via
    `sem_record_binding_types`; locals via the `SN_BIND_STMT` handler — both already wired).

## Layer 2 — presentation: read it, then hide the redundant ones (thin, also compiler-fed)

- `arche_analyzer.c` `emit_type_hint` stays the trivial reader: `sem_model_expr_type_id(node)` →
  render at the `:`/`=`. No computation, no kind logic. (done)
- Hide-redundant default: a FORM type (func/proc/sys/policy/archetype) already spelled by the source
  form is redundant → hidden unless full mode. Move the predicate into the **compiler** —
  `tyid_is_form_type(arena, t)` in `semantic/sem_types.{c,h}` — so the _categorization_ is a compiler
  fact too; the analyzer just calls it (replacing the inline `type_is_form` kind-check). Value types and
  the `type` meta (aliases, enums) show by default — `int : type : alias i32` is wanted. Opt-in full via
  `--dump --full` / `HINTS full` stays as built.

## Tests / verification / decisions

- **Completeness test** `tests/unit/language/explicit_view/binding_type_complete.arche`: one file with
  every binding kind (const, alias, enum, static scalar, func, proc, sys, policy, arche); assert
  `--dump --full` emits a `type` inlay for EACH (alias/enum → `type`, the rest their form/value type) —
  proving no kind is dropped. Plain `--dump` shows value + meta, hides the form signatures.
- Update `explicit_view/alias_backing.arche`: alias decls now show `: type :` (the user's ask) — adjust
  its expected hints (legit behavior change, logged), not a workaround.
- `make` (`-Werror`), `make test`, `make test-asan` all green; no duplicated diagnostics; decisions
  appended to `docs/TYPE_INLAY_DECISIONS.md` (the exhaustive-switch principle; meta-type for alias/enum;
  compiler-side redundancy predicate).

## Files

`semantic/semantic.c` (`sem_decl_type_id` exhaustive: +enum/array/func_group/world/use arms),
`semantic/sem_types.{c,h}` (`tyid_is_form_type`), `arche_analyzer.c` (call the predicate),
`tests/unit/language/explicit_view/*`, `docs/TYPE_INLAY_DECISIONS.md`.
