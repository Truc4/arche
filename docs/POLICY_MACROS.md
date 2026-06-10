# Failure policies as macros — design & autonomous decisions

Goal: **every failure policy is an ordinary `policy` decl (in `core` or user code) — nothing built
in, no intrinsics, no compiler-emitted bounds/abort logic.** The compiler keeps only a *mechanism*
(inline a policy at a fallible op) plus the irreducible `syscall` primitive. Everything green,
principled, no workarounds.

## The model

A `policy` is a **macro**. The compiler inlines its body at a fallible op, with the op's operands
bound as **mutable locals**, then runs the **raw** op on whatever they now are. There are two kinds,
split by whether the op has a way to *report* failure (see D17):

- **panic policies** (`@policy(bounds|divide)`, invoked `expr !name`) — for channel-less ops:
  - bounds index/slice → operands `len, i` ; the policy mutates `i` (or terminates), then `base[i]` raw.
  - divide → operands `a, b` ; the policy mutates them, then `a / b` raw.
- **handler policies** (`@policy(pool)`, invoked `insert(P,x) ?name (h:, ok:)`, or declared on the pool
  `Foo[N] ?name`) — for a pool insert, whose full-pool condition is an *expected* result reported via
  `ok`. Operands `count, cap, ok, slot` (the prefix the handler declares); the insert writes iff `ok`
  stays nonzero, into `slot` (redirect it to evict). See D18.

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
1. explicit `!policy` / `?policy` at the site;
2. build flag (`--unchecked` ⇒ `undefined`; `--no-abort` forbids an `abort` resolution);
3. the program's `@default(<kind>, <category>, <policy>)` directive for this op's `(kind, category)` cell;
4. baseline: bounds `func` → `clamp` / `proc` → `abort`; divide `func` → `zero` / `proc` → `abort`;
   pool → `reject` — these just *name* core policies.

A `@default` is a **standalone top-level directive**, not a per-decl decorator, so there can never be a
pile of defaults that shadow each other and go dead. It names a policy (core or user) for one
`(effect-kind, op-category)` cell, and a program may declare **at most one per cell** (a second is
**E0128 `duplicate_default`**). The named policy must belong to the stated category (else **E0124**); a
`func` cell cannot name `abort` (a func stays total — **E0129 `default_invalid`**); `pool` is `proc`-only.

```
@default(proc, bounds, clamp)   // a proc's unannotated index/slice clamps instead of aborting
@default(func, divide, zero)    // (already the baseline) — names it explicitly
@default(proc, pool,   evict_oldest)
```

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
- **D6. Defaults via a program `@default` directive + baseline.** The baseline names core policies;
  nothing hardcoded in codegen but the last-resort baseline mapping. A program may override a
  `(kind, category)` cell with a standalone `@default(<kind>, <category>, <policy>)` directive — see
  D20. (Superseded the old per-proc `@default(name)` decorator, which let many shadowing defaults
  coexist and go dead.)
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
- **D14. Divide default now wired.** Every integer `/` or `%` resolves a policy through `cg_policy_for`
  (category `divide`): explicit `a / b !zero` wins; else the program `@default(_, divide, _)`; else the
  baseline (`func`→`zero` total, `proc`→`abort`). So an unannotated div-by-zero is a visible policy
  decision — a proc aborts, a func yields 0 — never silent UB. Covers the compound-assignment path
  (`a /= 0`) too. Float and vector divides stay raw; a non-i32 integer divide also stays raw (a failure
  policy operates on `int`/i32 operands — widening the macro is future work). The shared helper is
  `emit_int_divmod`. (`policy/divide_default_{abort,func_zero,compound}*.arche`.)
- **D15. `&&` / `||` now genuinely short-circuit.** The old codegen evaluated *both* operands then
  bitwise-combined them — sound only while "arche expressions have no side effects and no traps." Failure
  policies broke that: a `!abort` (or any policy prelude) in a short-circuited-out operand fired anyway,
  aborting at a logically-unreachable site. Fixed with the standard branch lowering every modern compiler
  uses (clang's `land.rhs`/`lor.rhs`, Rust, GCC): branch on the LHS, evaluate the RHS — and the inlined
  policy prelude it carries — in its own block entered only on the taken path, merging the result through
  a stack slot (arche's value model is memory-backed, so no phi). Not a workaround: it *is* the canonical
  short-circuit lowering. Covered by `policy/midexpr/{and,or,divide_and}_shortcircuit.arche`.
- **D16. Slice reversed = the policy applied to the length.** The slice codegen used to emit a hidden
  `hi = max(hi, lo)` net-clamp so the length couldn't go negative — invisible to the policy, so
  `a[3:1] !abort` (reversed but in-range) silently became an empty slice and `!undefined` got the guard
  anyway. Removed: after applying the policy to `lo` and `hi` (each into `[0,N]`), the policy is applied
  a **third time to the length `hi - lo`** against `[0,N]`. So `clamp`→empty, `abort`→aborts on a
  reversed slice, `undefined`→raw negative length — all visible policy decisions, no special-casing.
  The last compiler-owned net-clamp is gone. (`policy/slice_reversed_{abort,clamp}.arche`.)
- **D17. The response shape follows the failure channel; that's what a policy is *for*.** A policy
  answers "what happens when an operand/condition leaves the valid domain?" — and the shape depends on
  whether the op has a way to *report* it. **No channel** (`a[i]`, `a[lo:hi]`, `a/b` return one value) →
  a **panic policy** (`@policy(bounds|divide)`, sigil `!`): crash / total-result / UB. **Has a channel**
  (`insert(P,x)(h:, ok:)` reports a full pool via `ok`, an `int`) → a **handler policy**
  (`@policy(pool)`, sigil `?`) shapes the *result*; the channel reports it, no crash. Don't police what a
  return value already reports: `delete` already splits this with no policy (stale handle = use-after-free
  *bug* → abort; generation-exhausted = expected → `ok=0`). `ok` stays an `int` and can widen to an error
  code later without a signature change.
- **D18. Pool overflow handlers (`?`): generic OR pool-specific, owned by the *pool* (storage), not the
  *archetype* (schema).** `Foo :: arche {…}` is the schema; `Foo[N]` is the pool; the handler attaches at
  `Foo[N] ?handler` because overflow is a storage concern. A handler is a macro the compiler specializes
  per insert site — arche's only way to write a *generic* overflow rule (one rule, any pool), since it has
  no archetype generics. Operands `(count, cap, ok, slot)` (the prefix a handler declares): `ok` init 1
  (the insert writes iff it stays nonzero), `slot` the write target (declare it to **redirect** → an
  eviction overwrites that slot; codegen bumps its generation so the evicted entity's handle goes stale,
  for free). A handler touching only operands is generic (`reject`, the default); one naming a pool's
  columns is pool-specific (`evict_oldest` scans `Foo.ts`). The capacity check lives in the handler (empty
  body = raw write). Resolution: per-call `?name` › pool-decl `?name` › `reject`. Sigil must match kind
  (`a[i] ?clamp` / `insert !reject` error). A handler reading a *different* archetype's columns than the
  insert target is W0019 (lint). Codegen: `reject` is the helper's built-in ok=0-on-full (zero overhead,
  passes `evict_slot = -1`); any other handler is inlined at the call site to compute `evict_slot`, which
  the shared `@arche_insert_<arch>` helper overwrites (reusing its write/gen-bump blocks).
  (`policy/pool_{reject,evict,evict_stale,override,cross_arch_lint}.arche`,
  `policy/errors/{handler_sigil_mismatch,insert_panic_sigil,pool_wrong_category}.arche`.)
- **D19. A live pool handle is never 0 — and handles compare at full width.** A handle is `slot |
  gen<<32`. Two fixes make `h == 0` an unambiguous overflow/failure sentinel even for a caller that
  checks `h` instead of `ok`: (1) the insert helper forces the *issued* generation to be ≥ 1 (`gen ==
  0 ? 1 : gen`, stored back so `delete`'s gen check matches), so even slot 0 yields a nonzero handle;
  and (2) a comparison with a handle operand is done at i64 (like opaque cells), not truncated to the
  i32 slot — which also fixes a latent bug where two handles to the same slot but different generations
  compared *equal*. `gen == 0` is never a free-slot marker (free slots live in the free_list), so
  promoting it is safe. (`policy/pool_handle_nonzero.arche`.)
- **D20. Program defaults: a standalone `@default(<kind>, <category>, <policy>)` directive, one per
  `(kind, category)` cell.** A failure-policy default is set by a *directive*, not a per-decl decorator
  (the old `@default(name)` on a proc/func let many shadowing defaults coexist and silently go dead).
  It is keyed by effect kind (`func` stays total — can't name `abort`; `proc` can) and op category, and
  references a policy *by name* so a core policy (`clamp`/`abort`/…) works without editing core. A
  program declares at most one per cell: a second is **E0128 `duplicate_default`**; a named policy of
  the wrong category is **E0124**; a `func`+`abort` or `func`+`pool` cell is **E0129 `default_invalid`**.
  Resolution (codegen `cg_policy_for` / `cg_insert_handler`): site `!name`/`?name` › the one `@default`
  for the cell › baseline. The directive emits no code — it only feeds the resolution table. Parsed as
  `SN_DEFAULT_DECL` → `HIR_DECL_DEFAULT`; validated in `sem_check_default_directives`.
  (`policy/default_decorator.arche`,
  `policy/errors/{duplicate_default,default_func_abort,default_category_mismatch}.arche`.)
