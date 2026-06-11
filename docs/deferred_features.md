# Deferred features & limitations

This is the honest catalogue of compiler work that is **deliberately not done yet** — surfaced by the
2026-06 audit. Each item is a *correctly-marked* gap (a clean compile-time error or a documented
limitation), not a stale lie. Items here are tracked for future plans; when one is built it gets a
WANT-asserting test that goes green (nothing is parked in a `known_failures` directory — that
mechanism is retired and both such directories are now empty).

The `func`/`proc` distinction this audit re-grounded on: **a `func` is a value, a `proc` does things**
(README.md "The function, split four ways"). Not "compile-time vs runtime."

## Real capability gaps (cleanly rejected today)

| ID | Site | Gap | Today's behavior |
|----|------|-----|------------------|
| **E2** | `semantic/semantic.c` (`SN_RETURN_STMT`) | **Return a local array by value** (array copy-out) | Rejected: "cannot return a local array by value". Needs the **sret ABI** (caller allocates the return slot, passes a hidden pointer, callee builds in place) — the same mechanism as the `TODO(sret)` at `codegen/codegen.c` (`HIR_STMT_RETURN`). Workaround: return an `own` parameter or thread a caller buffer (`g: T[N]; g := fill(move g)`). |
| **E3** | `semantic/semantic.c` (`analyze_static_array_decl`) | **Global array initializers** (`buf: T[N] = {…}`) | Rejected: "array initializers are not yet implemented". Needs (a) lowering `SN_ARRAY_LIT_EXPR` to a real HIR array literal (today an E6 placeholder in `lower/lower.c`), and (b) codegen emitting an initialized aggregate global (`@buf = global [N x T] [ … ]`) instead of `zeroinitializer` (`codegen/codegen.c`; `array.init` is currently never consumed). |
| **E5** | `lower/lower.c` (match lowering) | A `match` arm whose pattern is an unknown bare identifier / unrecognized token is silently `continue`d | Defensive fallback; the cases it drops are already rejected upstream in semantics. Low risk — listed for a confirming check, not a known miscompile. |
| **E7** | `lower/lower.c` (`lower_decl_cst`) | Tuple-flattening + field-init decl forms not ported into the CST path | Returns NULL for those forms; only reachable under the `ARCHE_LOWER_CST` migration toggle. Front-end-migration tech debt. |

## Roadmap features (intentional defers)

| ID | Site | Item |
|----|------|------|
| **E9** | `semantic/semantic.c` (`reject_meta_type`) | **Generics / monomorphization** — a parameter of meta-type `type` is rejected ("not supported yet"). |
| **E10** | `semantic/tycheck.c` | **Rich-TypeId expression checks** — the checker *fails open* on structural types (array / shaped / handle / tuple / archetype) because expression synthesis doesn't yet produce matching rich TypeIds. A soft coverage gap; needs golden updates. |
| **E11** | `codegen/codegen.c` | **Slice `.cap` through the call ABI** — a slice param's capacity falls back to its length. |
| **E12** | `codegen/codegen.c` (`HIR_STMT` call lowering) | **FBIP buffer reuse** — a producer needing a private result buffer could reuse a dead input's storage when aliasing is provably safe. Pure optimization. |
| **E13** | `codegen/codegen.c` (`HIR_STMT_RETURN`) | **sret aggregate return** — caller-allocate the bind target and pass a hidden out-pointer so a from-scratch aggregate result is built in the caller's slot. (Prerequisite for **E2**.) |

## CTFE scope (implemented; one position deferred)

CTFE (compile-time evaluation of a pure `func` of constants) is implemented for: **pool capacity,
pool init-size, `const` declarations, pool field-initializer values, and global scalar initializers**
(`semantic/semantic.c` `semantic_try_const_int`; lowered to a literal via `lower/lower.c`
`lower_const_or_expr`). Integer-only, straight-line func bodies (`:=` binds + `return`).

- **Deferred:** array **sizes inside a fixed-array type** (`char[N]`, `float[M][N]`). The type grammar
  only admits an integer-literal size (`parser/parser.c` "Expected ']' or integer size after '['");
  accepting an expression there is a layered change across parser grammar → type interning
  (`sem_types.c`) → lowering → codegen, i.e. a separate, larger feature. The other five constant
  positions cover the motivating cases. (Autonomous decision — see below.)

## Closed decisions

- **E8 — dynamic (runtime) allocation: out of scope for the core language, permanently.** Runtime-sized
  pools will **never** be a core feature; a future *library* provides dynamic archetypes on the same
  columnar model (README "No implicit heap"; `docs/language.md`). CTFE covers *constant* counts; the
  diagnostic now reads "alloc count must be a compile-time constant", with no "not yet" implication.
- **E14 — implicit move is implemented and intended.** A bare move-only name in an ownership-taking
  position is an implicit move (`semantic/semantic.c`, `lower/lower.c`, `semantic/sem_model.h`); the
  stale "requires explicit move/copy / TODO" comment in `sem_hints.h` was corrected.
- **E15 — unallocated-shape hard error is an intentional guard** (`run <sys>` over a shape with no
  driver-allocated pool would bind a null pool). Kept as a documented limitation tied to the
  device/shape model; revisit only if "driver-allocated shape storage" becomes a feature.

## Autonomous decisions (audit 2026-06-11)

Decisions made without round-tripping, per the "no hacks / fully green" directive:

1. **E2 and E3 deferred, not built.** The initial triage framed these as small "capability gaps."
   Deeper investigation showed each is a full codegen feature (E2 = the sret ABI across all call
   sites; E3 = new array-literal HIR + lowering + constant-aggregate global emission). A clean
   implementation is a sizeable, regression-prone effort; a partial/bolted-in version would violate
   "no hacks." Both are documented above with exact mechanisms and kept behind accurate compile-time
   errors. Recommended next: E3 first (more self-contained, pairs with CTFE), then E2 (depends on
   E13's sret).
2. **E4 was not a gap — it was a stale message.** Codegen already clones non-`char` `T[N]` locals;
   only the diagnostic claimed "only a local `char[N]` buffer." Reworded to the truth (only a local
   fixed-size `T[N]` copies; a parameter array or slice does not) and covered by
   `tests/unit/language/arrays/copy_nonchar_array.arche`.
3. **CTFE array-size-in-type deferred** (see "CTFE scope") — a layered parser/type change, separate
   from the five constant positions that were delivered.
4. **Dynamic-allocation doc framing kept.** `README.md` / `docs/language.md` already describe dynamic
   archetypes as *library, not core* — which is exactly the E8 stance — so that wording was kept; only
   the compiler error strings implying a pending *language* feature were scrubbed.
