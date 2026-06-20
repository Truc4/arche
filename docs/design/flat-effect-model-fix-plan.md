# Fixing the scheduling substrate to the value model

### what Phase 1 built, what the converged model needs, and the order to get there

A companion to [the-flat-effect-model](the-flat-effect-model.md). It records the gap between what
currently compiles and the model §9 now describes, and sequences the work to close it.

## The gap

The model converged on four load-bearing decisions:

- **`proc → proc` is the axiom** — a proc may not reach a proc, *transitively*. Everything functional
  exists to make composition possible without it.
- **Composition is values** — effects compose as `Eff` values (`func`-built, `|>`-composed), never nested
  calls.
- **Scheduling is a value** — the loop is a `Schedule` value the runtime executes; there is no driver loop.
- **There is no `main`** — a program is its declarations plus one `#run <Schedule>`; the runtime is the
  apex (the loop), not a kind.

Phase 1 shipped a **directive-based** scheduler instead: `#schedule { a b c }` + a `tick()` builtin +
`main :: proc()` driving the loop. That is the wrong substrate — it encodes "a proc paces the loop and the
schedule is a fixed list," exactly what the model removed. This plan replaces it.

## What survives from Phase 1

- **The `system` kind** — lexer `TOK_SYSTEM`, `SN_SYSTEM_EXPR`, `HIR_DECL_SYSTEM`, codegen as a no-arg void
  function. It *is* the composer; the body emitter is reused as-is. Keep.
- **`map`, pools, queries, singletons** — unchanged.
- The qualify-pass fix for `HIR_DECL_SYSTEM` bodies and the uninitialized-field fix — keep.

## What is removed or replaced

| Phase 1 artifact | fate |
|---|---|
| `tick()` builtin (codegen `@arche_tick` call, semantic guards E0064–E0067) | **removed** — there is no user tick; the runtime advances the schedule |
| `#schedule { … }` directive (`SN_SCHEDULE_DECL`, `HIR_DECL_SCHEDULE`, `parse_schedule_region`, `sem_check_schedule`, `@arche_tick` synthesis) | **replaced** by `#run <Schedule>` + the runtime executor |
| `tick` reserved-name check (E0068) | **removed** with `tick()` |
| `main :: proc()` as driver (`@main_user` wiring) | **removed** — the runtime entry runs the `#run` schedule |
| `tests/unit/language/schedule/*` (directive + tick tests) | **rewritten** for the value model |

E0063 `schedule_entry_not_runnable` survives in spirit as "a `run(x)` target must be a system/map," re-aimed
at the `Schedule` value.

## The corrected build order

The old Phase 1/2/3 split was backwards (it built the mechanism before its justifying axiom). The real
order, by dependency:

### Stage A — the `proc`-leaf axiom (check now, enforce later)

- **Semantic check:** a `proc` body may not contain a call to a `proc`. Permitted callees: `extern`,
  `func`, `map`. Because it is a *local* rule applied to every proc, the *transitive* ban falls out for
  free — if no proc calls a proc directly, none reaches one indirectly. (This is stronger than the existing
  E0050, which only forbids a proc call *nested in an expression*.)
- **Land as a tunable, default `warn`** (`--proc-leaf=warn|error`). It breaks essentially all current code
  — the stdlib, web-server, and rpg all call procs from procs — so it cannot be `error` until Stage B/C
  give a working way to express those programs without proc→proc. Flip to `error` in Stage D.
- New diagnostic in the registry.

### Stage B — the `Eff` value layer (the keystone)

- **`Eff(T…)`** = an `extern` under-applied by its out-slots (saturation); a pure `func` builds and returns
  it, running nothing. **`eff(out:)`** runs it (the single impure act). **`|>`** fmaps a pure `func` over an
  `Eff`'s result (applicative, not monadic — static, heap-free).
- This is the replacement for proc→proc composition. **PoC:** rewrite one stdlib pipeline
  (`open |> parse |> write`) end to end with zero proc→proc, proving the axiom is livable.
- Result-dependent composition (branch on a performed result) stays imperative in a `system` body — that is
  the applicative/value vs imperative/system line (§5).

### Stage C — the `Schedule` value layer + runtime executor

Replaces `#schedule` / `tick()` / `main` wholesale.

- **`Schedule`** — a recursive value type with the core constructors the runtime interprets: `run(s)`,
  `seq([]Schedule)`, `par([]Schedule)`, `loop(s)`, `when(pred, s)`, `halt`. `seq`/`par` take array literals,
  not variadic.
  - **Prerequisite to verify first:** `Schedule` must be expressible as a **recursive value type built as a
    static compile-time constant** (a node holding `[]Schedule` of its own kind), with system/predicate
    references as compile-time identities (the existing callback-by-name machinery). *That* representability
    — not variadic, not closures — is the real gate. Confirm it before building combinators.
- **`World`** — runtime-owned: clock, input queue, tick counter, core count. Predicates are pure
  `func(World) -> bool`.
- **`sched` library module** — `once`, `forever`, `at_hz`, `every`, `catch_up` as ordinary funcs over the
  core. User-extensible (any func returning `Schedule`).
- **`#run <Schedule>`** — one per program (a second `#run` is a duplicate-region error, like the old
  one-schedule rule). Names the program's schedule value.
- **The runtime executor** — walks the static `Schedule`, evaluates predicates against `World`, dispatches
  the guarded systems in composed order/parallelism. Parallelism in `par` is honored only within each
  system's pool read/write set (already known from `query{}`/pool access). This replaces `@arche_tick`.
- **Remove** `tick()`, the `#schedule` directive, and the `main`→`@main_user` driver wiring; the process
  entry (`@main`) runs the `#run` schedule.
- **Rewrite** `tests/unit/language/schedule/` for `#run` + the combinators.

### Stage D — final restrictions (enforce the axioms)

- Flip `--proc-leaf` to `error` (default).
- **system → system** ban; a `system` is **dispatched only** (not callable as a statement — `run frame`
  already rejected; extend to "never reachable except through a `Schedule`").
- **Export rule:** `proc`/`extern` private; `func`/`func→Eff`/`system`/`pool`/`type` public.
- **Device contributes systems** to the global namespace; no callable device surface.

## Honest notes on sequencing

- **The axiom can't bite until its replacement exists.** Stage A ships the *check* (warn); Stage D flips it
  to error — but only after B (Eff) and C (systems + schedule) give programs a way to compose effects and
  pace loops without proc→proc. Turning it on earlier just breaks everything with no alternative.
- **`#run` can't replace `#schedule` until Stage C lands.** Until then Phase 1's `#schedule`/`tick` remain as
  a *frozen bridge* — no new work on them, no new features, just "still runs" so systems are testable while
  C is built. They are deleted at the end of C, not before.
- **No `main` is the largest user-visible break.** Every program (the slice, web-server, rpg) currently has
  `main :: proc()`. They migrate to `#run`. The migration docs show the shape.
- **The real risk in C is representability, not syntax.** If recursive value types as static constants don't
  yet work, *that* is the prerequisite to build first — variadic is a non-issue (array literals suffice).

## Out of scope (model §9, "still genuinely open")

The exact `World` primitive set; truly independent concurrent timelines (`par` is within-tick only); any
fundamentally new dispatch primitive (preemption, I/O-completion) that isn't a `func` over the core.
