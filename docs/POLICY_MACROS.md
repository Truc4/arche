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
- **`abort`** = a policy that **terminates** on a bad operand. Termination is `exit(134)` via the
  `syscall` intrinsic — there is **no `abort()` primitive**; "crash" is just write+exit, pure arche.
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

## The one primitive

`syscall` (the raw instruction) is hosted in `core` exactly like `os` hosts it. `exit` is a normal
`proc` over it. Nothing else in the policy system is compiler-special.

## Autonomous decisions (this rewrite)

- **D1. Policies inline (not call).** Required so an empty body ⇒ raw op, and so a divide policy can
  mutate *both* `a` and `b`. Inlining reuses normal statement codegen + scopes (arche scalars are
  SSA-rebind and already phi-correct across branches/loops, verified). Policy decls emit **no** LLVM
  function — they're templates read at each op site.
- **D2. No `abort()` primitive.** `abort` is a policy that calls `exit(134)`; `exit` is `syscall(231,…)`.
  Avoids the libc `@abort` name clash and keeps "crash" in pure arche. 134 mirrors the old SIGABRT exit
  code so the `*_array_oob` `! %t` tests still pass.
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
- **D9. `_exit`, not `exit`.** The terminate primitive is libc `_exit` (also real) so its global name
  doesn't clash with a device's `os.exit` in the all-stdlib codegen-test harness.
- **D10. Slices: the policy's `len` is `base_len + 1`.** A length-N slice has N+1 boundary positions
  (0..N); inlining the index policy on each of `lo`/`hi` with `len = N+1` yields exact slice semantics
  (`clamp`→[0,N], `abort`→aborts past N). An unknown base length (`-1`, a foreign raw pointer) is
  trusted (raw view) — preserving the pre-policy behavior for FFI slices.
- **D11. core hosts a `#foreign` region** (for `_exit`); the one-`#foreign`-per-file rule exempts the
  prepended-prelude region (by line offset) so a user file may still have its own.
- **D12. `%` (modulo) added** as a real operator (`srem`/`urem`/`frem`, prec 5, `%=` compound) — `wrap`
  needs it and it was simply missing.
