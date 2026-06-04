# type_identity — distinct-by-default nominal types

These tests are LIVE in the suite (no longer staged). They pin the two-tier type-identity model.

## The model (two tiers)

- **`name :: Backing`** — SUBTYPE (tier-2, the unmarked DEFAULT). A distinct nominal type, usable AS
  its backing one-way: `N→B` is implicit; `B→N` and `N→M` (sibling) need an explicit `N(x)`.
  Operators/literals are exempt (a literal adopts the subtype; `meters + seconds` is a `meters` — the
  left operand's nominal). `opaque` is an ordinary backing: `file :: opaque` makes `file`≠`socket`
  via the SAME mechanism (opaque cells, being foreign-forged, still flow freely from a raw
  opaque/int — only distinct opaque siblings are rejected).
- **`name :: alias Backing`** — ALIAS (tier-1, transparent). Same type identity as the backing,
  interchangeable both ways. `alias` is a soft contextual keyword (only the leading word of a `::`
  RHS). Core's `int`/`byte`/`bool` and enums are tier-1.
- Codegen always erases a subtype to its backing; distinctness is a substitution-boundary property.

## Tests

| test | asserts |
|---|---|
| `subtype_sibling_rejected` | `meters`→`seconds` param rejected (`expected 'seconds'`) |
| `subtype_backing_needs_conv` | bare `float`→`meters` slot rejected (`expected 'meters'`) |
| `subtype_convert` | `meters(f)` / `float(d)` conversions type-check + run (`r=7`) |
| `subtype_operators` | same/cross-backing + literal ops → left nominal (`a=30 b=15 c=8`) |
| `subtype_usable_as_backing` | `meters` usable where `float` expected (`got 5`) |
| `alias_transparent` | `dist :: alias float` interchangeable both ways (`r=3`) |
| `opaque_unified` | `handle_a`/`handle_b` distinct, unified `expected 'handle_b'` message |
| `component_distinct` | cross-component arithmetic (`mass*charge`) allowed (`e=50`) |
| `component_sibling_rejected` | `mass`→`charge` slot rejected (`expected 'charge'`) |
| `core_int_byte_bool_transparent` | `int`↔`i32`, `byte`↔`u8` transparent (`v=5`) |
| `units_program` | end-to-end units program with `mps(...)` conversion (`speed=4`) |
| `archetype_components_smoke` | distinct components don't disturb pools (`total=100`) |

## Implementation touch-points

- parser.c — `alias` consumed in the `::` const/binding RHS (loose CST token).
- cst.h / semantic.c — `ConstDecl.is_transparent`; `cst_const_alias_marked`; per-alias tier in the
  registry (`register_type_alias_tiered`, `alias_is_transparent`); qualified-aware `alias_name_matches`.
- tycheck.c — `tyid_from_name` interns tier-2 by name; `subtype_check` (N→B ok, B→N/sibling reject,
  opaque forge leniency); literal fits a subtype's backing; binary result = left nominal; `T(x)`
  conversion result; `move`-transparent synth.
- sem_model — parallel `expr_nominal` channel (tycheck sees the subtype; lowering keeps the backing).
- semantic.c / codegen.c — `T(x)` recognized as a conversion (analyze arg only); codegen converts the
  arg to the resolved backing (identity for same-backing).
