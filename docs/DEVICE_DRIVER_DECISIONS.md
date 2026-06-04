# Device/Driver implementation — autonomous decision log

Decisions made while implementing the device/driver model (plan: `~/.claude/plans/load-up-the-worlds-glowing-wren.md`). Each entry: the decision, why, and how to revisit. User-made decisions are noted as such; the rest are autonomous.

## Naming / scope
- **`device` / `driver` / `datasheet` vocabulary** (user). A library = a *device* exposing a `.i.arche` *datasheet*; the program = a *driver* implementing devices. Renamed compiler `driver/` → `compile/` to free the word.

## Part 1a — qualified pool decls
- **Dotted name reassembled in BOTH `lower.c` and `semantic.c`.** The semantic analyzer builds its own model from the CST (separate from HIR lowering), so the qualified-name fix (`lib.Particle` from the `.`-joined IDENTs before `[`) had to be applied in both. (autonomous)
- **No-silent-conflict reused the existing guard.** Two pool decls for one canonical shape already error via semantic's "Shape already allocated" check — no new code; covered by a negative test. (autonomous)

## Part 1b — anonymous shape literals
- **Synthetic-archetype minting.** An anonymous `arche{…}` literal lowers to a `NAME` referencing a synthetic global archetype (deterministic name `__shape_<sorted fields>`, deduped), registered in `ast->decls`; `canonical_archetype_decl` then folds it onto a structurally-identical named shape. (autonomous, matches plan)
- **`SN_ARCH_EXPR` added to the call-arg kind set.** It sits outside the contiguous `[SN_LITERAL_EXPR..SN_PAREN_EXPR]` range, so `insert(arche{…}, …)` was silently dropping the literal arg; explicitly accepted it. (autonomous)

## Part 2 — cross-module `run device.system`
- **Run system name reassembled like pool names**, joining IDENT segments with `.` in both paths. The dormant `in world` plumbing is not emitted by the parser, so `world_name` is set NULL. (autonomous)

## Part 2 — `@implements` binding
- **Lowering strategy = name-substitution at lowering** (user choice), mirrored in semantic.

## KEY SCOPE DECISION — two models, one shipped (autonomous)
Empirically probing the device/driver model surfaced two distinct shapes it can take:

- **Model A — driver uses the device's shape (SHIPPED, green).** The device owns its shapes + systems; the driver sizes them by qualified name (`device.Shape[N]`, Part 1a), inserts, and runs the device's systems by qualified name (`run device.system`, cross-module run). This *is* the original goal of the whole effort — "a library defines shapes + systems but never allocates a pool; the caller provides the storage and picks the size." It works end-to-end and is covered by `tests/unit/language/devices/`.

- **Model B — driver owns the shape; device declares requirements (DEFERRED, documented).** The `.i.arche` datasheet + `@implements` name-mapping, where the driver provides its OWN shape/components and binds them to the device's required names. Probing showed this needs **global cross-module component identity**: a device's `sys (pos, vel)` must bind its columns to the *driver's* archetype, and components must be shared (not module-prefixed) across the boundary. Today import prefixes them (`dev.pos`), and a device system with no owned archetype produces garbage/over-flow at runtime. Making components global, and re-binding device systems to driver archetypes, is a **deep change to the module naming/visibility system** — exactly the work the plan itself flags as "design-stage … needs a focused detailing pass before coding."

**Decision:** ship Model A as the device/driver implementation; keep Model B as a designed follow-up with `known_failures/implements_binding.arche` documenting the target. Rationale: Model A satisfies the stated goal and is fully green; Model B's global-component redesign is multi-step and risks the 427-test green suite — it must not be rushed in a way that fakes green (the standing "no workarounds" rule). When Model B is built, the datasheet loader (unprefixed `.i.arche` decls) + a column/name re-bind pass at device→driver inlining are the two pieces to add.

## Part 2 — bounded pieces shipped
- **`#plugin` banner** = alias for `#module` (TOK_HASH_MODULE) — device-private visibility, the device/driver spelling. (autonomous, plan). Test: `devices/plugin_visibility.arche`.
- **Archetypes global-only (guard).** A nested `Local :: arche { … }` (a named archetype binding inside a proc/block) is now a parse error; anonymous `arche { … }` literals in expressions stay allowed (Part 1b). Implemented in the parser via `cst_single_node_kind` on the binding RHS — the one place with clean error access. Covers the proc-local case (the common one); an archetype inside a `#foreign`/`#module` *block* region is not yet rejected (edge case, logged as remaining). Test: `devices/archetype_global_only.arche`.
- **Tuple-group / component conflicts: already covered.** Conflicting tuple groups (`pos(x,y)` vs `pos(x,y,z)`) and duplicate components are already caught by semantic's existing component-uniqueness check ("component … defined more than once") — no new code, same as the pool-conflict guard reusing "Shape already allocated". Verified empirically.

## Part 3 — `arche init` (shipped)
- **`arche init device <name>` / `arche init driver <name>`** scaffold templates (`cli/cmd_init.c`). Templates are **validated working**: the generated device's doctest passes (`arche test`) and the generated driver builds + runs. Never overwrites an existing file (`fopen "wx"`). Test: `tests/unit/cli/init.arche`. Decision: templates use Model A (driver uses the device's shape by qualified name), since that's the shipped model.

## Part 4 — docs (shipped)
- `docs/devices.md` (the model + what's shipped vs deferred), `docs/tooling.md` (`arche init`), README pointer, this decision log. `WORLDS_PLAN.md` was already removed from the tree (nothing to retire). `docs/GRAMMAR.peg` / `language.md` deeper sections: remaining, low-risk follow-up.

## Remaining (documented, not shipped)
- **Model B** (datasheet `.i.arche` + `@implements` + global cross-module components) — the deep item; see the scope decision above. Smoke test: `known_failures/implements_binding.arche`.
- **Archetype in a `#foreign`/`#module` block region** — not yet rejected by the global-only guard (the proc-local case is). Edge case.
- **`docs/GRAMMAR.peg` / `language.md`** device/driver sections — narrative docs exist in `devices.md`; grammar/reference updates pending.
