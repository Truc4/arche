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
- **`#module` banner** = device-private visibility (TOK_HASH_MODULE). Test: `devices/plugin_visibility.arche`. (A `#plugin` alias was briefly added then reverted — `#module` is the one spelling; a device is a module that also ships a datasheet, so the general term wins.)
- **Archetypes global-only (guard).** A nested `Local :: arche { … }` (a named archetype binding inside a proc/block) is now a parse error; anonymous `arche { … }` literals in expressions stay allowed (Part 1b). Implemented in the parser via `cst_single_node_kind` on the binding RHS — the one place with clean error access. Covers the proc-local case (the common one); an archetype inside a `#foreign`/`#module` *block* region is not yet rejected (edge case, logged as remaining). Test: `devices/archetype_global_only.arche`.
- **Tuple-group / component conflicts: already covered.** Conflicting tuple groups (`pos(x,y)` vs `pos(x,y,z)`) and duplicate components are already caught by semantic's existing component-uniqueness check ("component … defined more than once") — no new code, same as the pool-conflict guard reusing "Shape already allocated". Verified empirically.

## Part 3 — `arche init` (shipped)
- **`arche init device <name>` / `arche init driver <name>`** scaffold templates (`cli/cmd_init.c`). Templates are **validated working**: the generated device's doctest passes (`arche test`) and the generated driver builds + runs. Never overwrites an existing file (`fopen "wx"`). Test: `tests/unit/cli/init.arche`. Decision: templates use Model A (driver uses the device's shape by qualified name), since that's the shipped model.

## Part 4 — docs (shipped)
- `docs/devices.md` (the model + what's shipped vs deferred), `docs/tooling.md` (`arche init`), README pointer, this decision log. `WORLDS_PLAN.md` was already removed from the tree (nothing to retire). `docs/GRAMMAR.peg` / `language.md` deeper sections: remaining, low-risk follow-up.

## Model B — Phase A: `.i.arche` datasheet (SHIPPED, green)
- **Decls in a `*.i.arche` file register global/unprefixed.** Threaded the source filename into `LowerModule`/`SemModule` (was dropped); `hir_add_module_decl`/`sem_add_module_decl` skip the rename `full`-set for datasheet decls so they stay bare, and export them as `name=name` (bare) so both `pos` and `physics.pos` resolve to the one global. Confirmed empirically: a device's `sys(pos,vel)` binds to the driver's `Thing{pos,vel,mass}` by column name with **no binding-logic change** — exactly as the Phase-A research predicted. Test: `devices/device_datasheet.arche` + `devices/libdev/` (datasheet + systems). (autonomous, per approved plan)
- **Key realization (logged):** Model B's same-named-component case needs *only* global components — no `@implements`. `@implements` (Phase C) is purely the differently-named (`foo`→`bar`) layer.

## Model B — Phase B: diagnostics (SHIPPED, green)
- **`run <system>` with no shape providing its components is now a hard build error** (was a silent no-op). The matcher already lives in codegen `HIR_STMT_RUN` (`codegen.c:6396`); turned the `; WARNING` into a real stderr error + a `CodegenContext.had_error` flag that `compile.c` checks after `codegen_generate` (codegen returns void, so a flag + `codegen_had_error()` accessor was the minimal propagation). Unknown-system `run` also errors now. Done in codegen (not semantic) because semantic has no system registry and a definition-side check would wrongly fire on imported-but-unused device systems. Tests: `devices/run_unsatisfied.arche`, `devices/datasheet_redefine.arche`.

## Model B — Phase C: `@implements` name-mapping (SHIPPED, green)
- **`@implements(<device>.<req>, …)` on a driver decl rebinds the device's `req` → the decl's name.** Implemented as a post-inlining substitution that *reuses the rename traversal*: a global `g_impl` map checked first inside `prefixed_dup` (set-independent, so it fires even though datasheet decls aren't in any rename set). The apply pass drops the datasheet requirement decl (the driver's decl is the real definition) and substitutes `req → driver-name` everywhere; sys param names + archetype field names (which the traversal skips, but column binding needs) are substituted explicitly via `subst_name`. **HIR-only** — semantic tolerates the un-substituted device sys (no matching archetype → untyped, no error), and codegen uses the substituted HIR. (autonomous)
- **Tuple-group vs decorator collision (found + fixed):** `@implements(dev.foo)` looks like a tuple group `implements(dev,foo)` to `build_tgroups` and semantic's const-decl builder. Added a `decl_is_decorated` / `sem_decl_is_decorated` guard (skip tuple-group detection for decorated decls) and switched the semantic const-name to `sem_binding_name` for decorated decls (the first IDENT is the decorator, not the name). This also hardened `@allow`/`@drop` against the same latent confusion. Test: `devices/implements_binding.arche` (promoted from `known_failures/`).

## Model B status: COMPLETE
The full device/driver system is shipped and green (439 tests). Both models work: Model A (driver uses the device's shape by qualified name) and Model B (driver owns the shape; datasheet shares components globally; `@implements` maps differing names).

## Storage requirements — datasheet minimums + driver-owned pools + `arche fill` (SHIPPED, green)
The storage/sizing thread (the "router's TrieNode" problem): a device needs storage for its shapes, but the driver owns all sizing. Resolution, all **user decisions**:

- **Requirement, not default.** A *size* is one number per pool (one pool per shape), always the driver's. A *device* states a **minimum requirement**; a size never attaches to a shape (shapes are shared/aliasable, so "the shape's default" is incoherent). Requirements **compose by `max`** across devices, so a shared shape has no ambiguity.
- **It's a datasheet, so minimums belong in it.** A pool decl inside a `.ds.arche` is a *requirement* (min rows), not an allocation. Same syntax, meaning keyed off file kind — zero new grammar.
- **`.i.arche` → `.ds.arche`** (user). The datasheet extension is `.ds.arche` ("ds" = datasheet); fits the device/driver (hardware) metaphor better than an `.mli`-style "interface".
- **Datasheet = REQUIREMENTS, not surface** (user). The datasheet states what the device requires the driver to provide (components + storage minimums), NOT what the device exposes. The device's API stays on the existing export set (calling) + `#module` regions (hiding) — listing it in the datasheet would be redundant twice over. So datasheet-as-public-API was dropped; the `contracts/device_contract.arche` premise belongs to the separate contract-enforcement effort, not here.
- **`#plugin` reverted** (user). `#module` is the one privacy banner; a device is a module that also ships a datasheet, so the general term wins. The `#plugin` alias added in Part 2 was removed.
- **Driver owns every pool, in driver source.** No device-side pool *allocations*; the datasheet minimum is only a check. Enforcement (semantic): a driver pool `< min` errors; a required shape with no driver pool is a hard error pointing at `arche fill`; two driver pools for one shape keep the existing "Shape already allocated". A `is_requirement` flag on the static decl (HIR + sem) carries the datasheet origin; codegen skips requirements (emit no storage). Composition + the missing/under-size sweep live in `sem_check_storage_requirements` (new `ArchetypeInfo.min_rows`/`alloc_capacity`/`req_count`).
- **Accept sharing, make it clear** (user). Two devices using the same `{key,val}` shape fold to one pool; identical datasheet vocabulary (components/shapes) **dedups silently** instead of tripping define-once (E0045) — a `from_datasheet`/`is_datasheet` flag threaded through `register_type_alias_tiered` (incl. the deferred-alias path) and the inline-archetype-field loop, plus a HIR dedup of duplicate datasheet archetype decls (one shape = one struct). A build **note** fires when ≥2 datasheets require a shape. Both declaration styles work: inline-typed fields and pre-defined components.
- **`arche fill` + `arche init driver <name> <dev>...`** (user: "both"). `cli/cmd_fill.c` reads imported devices' datasheets, composes minimums by `max`, and appends a `Shape[min]` pool for each required shape the driver doesn't already size — idempotent, into the driver's own source. `arche_fill_driver()` is shared by `fill` and `init driver`.
- **Doctest limitation (logged).** A device that declares its shape in its datasheet can't drive that shape from its *own* doctest — doctests run single-file and don't merge the sibling `.ds.arche`. So `arche init device` stays Model A (shape in the impl, doctest works); the datasheet/requirement model is for cross-device sharing. Revisit if doctests gain folder context. **[RESOLVED below — device doctests now compile as generated drivers.]**

## Imports + stdlib→devices + rule 3 + device doctests + fill=sum (SHIPPED, green — 465 lit + doctests + C units)
Follow-up round, all **user decisions**:

- **Import a device by name, a plain module by path.** A bare name in `#import { x }` must resolve to a device (a unit with a `.ds.arche`); a plain module is imported by a **string path** (`#import { router "./util" }`). Parser accepts `TOK_STRING` in the `#import` block/bare forms; `compile.c` `load_module_from_path` resolves a path relative to the importer's dir (module name = basename sans `.arche`); `import_token_module_name`/`sem_import_token_module_name` mirror this in the lower/semantic inlining walks. Rule-1 enforcement (`g_resolve_errors` in compile.c, checked by `compile_frontend`): a bare-name import of a non-device errors "import by path". `core` exempt. Non-device test fixtures (phys, plugin_dev, xmod_*, nsmod_*) converted to path-imports.
- **fill = SUM, requirement = MAX** (user: "requirement is MAX, generated is SUM"). The compiler's enforcement floor stays `max` (each device's own min must be satisfiable); `arche fill` writes the **sum** across devices that share a shape (they coexist), with a `// dev: n, dev: n` contributor comment. (`cli/cmd_fill.c` `ReqEntry` records contributors; sums.)
- **Every stdlib module is a device.** All 10 (`csv fmt http io net os parse router str term`) ship a `.ds.arche`. Pure-behavior modules get a marker datasheet; `io.file`/`net.socket` opaque types moved to their datasheets; **router** moved its `nodeKind`+`TrieNode`+`TrieNode[256]` to `router.ds.arche` (requirement) and stopped self-allocating — the driver owns the trie pool now (router test allocates `TrieNode[256]`).
- **Component == type, types are global vocabulary** (user: "there is no difference between a component and a type"). So datasheet decls register **global/bare** uniformly (sharing requires it). `io.file`/`net.socket` are therefore the bare types `file`/`socket`; the ~26 qualified usages were rewritten to bare. (This supersedes the earlier "datasheet=requirements not surface" wrinkle: a device's types DO live in its datasheet, as global vocabulary.)
- **Rule 3: a device's impl is behavior-only.** A decl from a device's `.arche` (a unit that also has a `.ds.arche`) may not be a type/enum/archetype definition or an archetype pool allocation — those belong in the datasheet / are the driver's. `from_device_impl` flag on `Decl` (set in `sem_add_module_decl` when `module_is_device && !is_datasheet`); `sem_check_device_impl_decls` sweep rejects them (type detection via `is_type_alias`). Buffers/scalars/value-consts allowed.
- **Device doctests compile as generated drivers** (user). `doctest_run.c` `read_device_context`: when a doctested file's folder has a `.ds.arche`, `synthesize` concatenates the whole device folder (datasheet + impl) + the example, so the datasheet's `Shape[N]` becomes the driver's real allocation (not module-inlined → rule 3 doesn't fire) and the example drives the device's shapes with no manual sizing. This RESOLVES the doctest limitation above, so **`arche init device` now scaffolds the shape in `<name>.ds.arche`** (behavior + doctest in `<name>.arche`).

## Contract rules — final shape (all three contracts/ tests now PASS)
- `import_non_device.arche` → rule 1 (device by name / module by path).
- `stdlib_are_devices.arche` → rule 2 (every stdlib module has a `.ds.arche`).
- `device_no_types_or_alloc.arche` → rule 3 (device impl behavior-only; leakdev's impl `handle :: opaque` errors).

Tests: `tests/unit/language/devices/storage/` (requirement met, below-min error, missing-pool error, shared-shape max+note, inline + pre-defined styles) and `tests/unit/cli/fill.arche` + `init.arche`. Suite green (459, minus the 3 pre-existing expected-fail `contracts/` tests for the separate enforcement effort).

## Remaining (documented, not shipped)
- **Contract enforcement** (only-devices-importable, device-sticks-to-datasheet) — the `contracts/` expected-fail tests; a separate effort from storage. The datasheet-as-surface premise was reframed (see above).
- **Module/visibility rethink** (user: "leave it for now") — `#module` vs `#file` are currently identical in the export path; public-by-default vs `#export`; whether all importable modules must be devices. Deferred.
- **Archetype in a `#foreign`/`#module` block region** — not yet rejected by the global-only guard (the proc-local case is). Edge case.
- **`docs/GRAMMAR.peg` / `language.md`** device/driver sections — narrative docs exist in `devices.md`; grammar/reference updates pending.
