# Failure policies as macros — design & autonomous decisions

Goal: **every failure policy is an ordinary `policy` decl (in `core` or user code) — nothing built
in, no intrinsics, no compiler-emitted bounds/abort logic.** The compiler keeps only a *mechanism*
(inline a policy at a fallible op) plus the irreducible `syscall` primitive. Everything green,
principled, no workarounds.

## The model

A `policy` is a **macro**. The compiler inlines its body at a fallible op, with the op's operands
bound as **mutable locals**, then runs the **raw** op on whatever they now are:

- bounds index/slice → operands `len, i` ; the policy mutates `i` (or terminates), then `base[i]` raw.
- divide → operands `a, b` ; the policy mutates them, then `a / b` raw.

Consequences that "fall out" of the model (no special-casing):
- **`undefined`** = a policy with an **empty body**: no mutation ⇒ the raw op. Not a keyword.
- **`abort`** = a policy that **terminates** on a bad operand: it writes a diagnostic to stderr
  (`dprintf(2, …)`) then `_exit(134)`. Both are libc externs in `core`'s `#foreign` block — there is
  **no `abort()` primitive**; "crash" is just write+exit, pure arche.
- **`clamp` / `wrap`** = policies that mutate `i` into range.
- The old compiler **net-clamp is gone** — a policy owns its own correctness (it's a macro; a buggy
  user policy can produce an OOB, exactly like buggy hand-written code would).

## Defaults are declared, not hardcoded

An unannotated fallible op resolves its policy by precedence:
1. explicit `!policy` at the site;
2. `@default(<policy>)` on the enclosing proc/func (per-decl);
3. build flag (`--unchecked` ⇒ `undefined`; `--no-abort` forbids an `abort` resolution);
4. baseline: `func` → `clamp` (stays total), `proc` → `abort` — these just *name* core policies.

A **proven-safe** op (the bounds prover discharged it) carries no policy at all — nothing is inlined.

## The primitives

`core`'s `#foreign` block declares two libc externs the policy system needs: `_exit(code)` (terminate)
and `dprintf(fd, fmt)` (the stderr diagnostic). Nothing else in the policy system is compiler-special.

## Autonomous decisions (this rewrite)

- **D1. Policies inline (not call).** Required so an empty body ⇒ raw op, and so a divide policy can
  mutate *both* `a` and `b`. Inlining reuses normal statement codegen + scopes (arche scalars are
  SSA-rebind and already phi-correct across branches/loops, verified). Policy decls emit **no** LLVM
  function — they're templates read at each op site.
- **D2. No `abort()` primitive.** `abort` is a policy that calls `dprintf(2, "arche: …\n")` then
  `_exit(134)` — both libc externs in core. Keeps "crash" in pure arche; 134 mirrors the old SIGABRT
  exit code so the `*_array_oob` `! %t` tests still pass. The stderr line names the failure
  (`index out of bounds` / `divide by zero`) so a crash isn't silent.
- **D3. `undefined` is a real empty policy in core**, not a recognized keyword. `!undefined` inlines
  nothing ⇒ raw op.
- **D4. Net-clamp removed.** Policies own correctness. The bounds prover still rejects provably-OOB
  *constants* and still elides proven-safe ops (no policy inlined there).
- **D5. bounds-read-`zero` dropped.** "Return 0 from an OOB array *read*" isn't operand mutation (no
  slot is guaranteed 0) — it can't be a prelude macro and nobody needs it (use clamp/wrap). Divide
  `zero` *is* operand mutation (`b=1; a=0`) and stays.
- **D6. Defaults via `@default(name)` decorator + baseline.** The baseline (proc→abort, func→clamp)
  names core policies; nothing hardcoded in codegen. `@default(clamp) p :: proc(){…}` overrides.
- **D7. Inliner uses alloca-backed operands.** The policy's params are bound as `type==1` mutable
  scalars (alloca + store), exactly like a normal `i := …` local, so branch reassignments (`if(i<0){i=0}`)
  store/load correctly. A plain-SSA binding silently dropped a reassignment made inside an `if` (the
  first bug found). Multiple policies in one expression compose because each mutates its own *copy* —
  `foo[i] + bar[i]` clamps `i` separately per array; the source `i` is never touched (verified).
- **D8. Policies are a separate namespace.** A `policy` and a `func`/`proc` may share a name (the user
  `zero` func and core's `zero` divide policy coexist): `name(...)` resolves to the callable
  (`find_callable_sig`/`find_func_decl` skip policies), `!name` resolves to the policy
  (`find_policy_sig`/`find_policy_decl`). Duplicate-decl checks exempt policies.
- **D9. `_exit`/`dprintf`, not `exit`/`write`.** The terminate + diagnostic primitives are libc `_exit`
  and `dprintf` — chosen so neither global name clashes with a stdlib symbol (`os.exit`, `os.write`) when
  the codegen-test harness registers every stdlib module flat in one scope. (`dprintf`'s format carries
  no `%`, so it is a plain fd-write; the messages have no varargs.) Separately, the duplicate-decl check
  now skips any pair involving a `DECL_ORIGIN_STDLIB` decl: a stdlib symbol is module-qualified
  (`os.write`) and never duplicates a global/core/user name — they only appear flat together in that
  harness, never in real resolution.
- **D10. Slices: the policy's `len` is `base_len + 1`.** A length-N slice has N+1 boundary positions
  (0..N); inlining the index policy on each of `lo`/`hi` with `len = N+1` yields exact slice semantics
  (`clamp`→[0,N], `abort`→aborts past N). An unknown base length (`-1`, a foreign raw pointer) is
  trusted (raw view) — preserving the pre-policy behavior for FFI slices.
- **D11. core hosts a `#foreign` region** (for `_exit`); the one-`#foreign`-per-file rule exempts the
  prepended-prelude region (by line offset) so a user file may still have its own.
- **D12. `%` (modulo) added** as a real operator (`srem`/`urem`/`frem`, prec 5, `%=` compound) — `wrap`
  needs it and it was simply missing.
- **D13. Category check (`@policy(bounds|divide)`).** A policy decl carries its op category, threaded
  through HIR (`policy_category`), lowering, and codegen. Resolution is keyed by **name AND category**,
  so `abort` and `undefined` exist as *both* a bounds policy and a divide policy and the op site selects
  which (`find_policy_decl(name, category)` / `find_policy_sig_cat`). Validation (`validate_explicit_policy`)
  errors on a category mismatch: `a / b !clamp` (E0124, bounds policy on a divide) and `a[i] !zero`
  (divide policy on a bounds site) are both rejected instead of silently degrading to the raw op. The
  former "intrinsic name" whitelist is gone — `abort`/`undefined`/`zero`/`clamp`/`wrap` are all ordinary
  categorized core decls; only the name-based rules (`abort` forbidden in a `func`; `--no-abort` /
  `--no-undefined` flags) remain special. Codegen's divide site inlines with the `divide` category;
  index/slice sites with `bounds`.
- **D14. Divide default left explicit.** A `/` or `%` only inlines a policy when one is written
  (`a / b !zero`); an unannotated divide is the raw op, unchanged. `cg_policy_for` *can* supply a divide
  default (func→`zero`, proc→`abort`) but isn't yet wired to every division — deferred to avoid the broad
  behavioral change of checking every divide. Bounds defaults (func→`clamp`, proc→`abort`) are active.
- **D15. `&&` / `||` now genuinely short-circuit.** The old codegen evaluated *both* operands then
  bitwise-combined them — sound only while "arche expressions have no side effects and no traps." Failure
  policies broke that: a `!abort` (or any policy prelude) in a short-circuited-out operand fired anyway,
  aborting at a logically-unreachable site. Fixed with the standard branch lowering every modern compiler
  uses (clang's `land.rhs`/`lor.rhs`, Rust, GCC): branch on the LHS, evaluate the RHS — and the inlined
  policy prelude it carries — in its own block entered only on the taken path, merging the result through
  a stack slot (arche's value model is memory-backed, so no phi). Not a workaround: it *is* the canonical
  short-circuit lowering. Covered by `policy/midexpr/{and,or,divide_and}_shortcircuit.arche`.
