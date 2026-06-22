# Scheduling

A program has no `main` and no hand-written driver loop. It is its declarations plus one `#run <Schedule>`
value; the runtime owns the loop. `Schedule` is a first-class value built from a small core (`core.arche`):

```arche
Schedule :: sum {
  seq([]Schedule)  par([]Schedule)  loop(Schedule)
  when(func() -> bool, Schedule)  halt
}
```

- A **leaf is a bare map/each/system name** — `#run seq({ boot, render })` dispatches them in order. There is
  **no `run` constructor** and **no `run` statement** (both retired); `map`/`each` are kinds of systems and
  are scheduled by name. A system body never dispatches another.
- `seq({…})` / `par({…})` — children in order (`par` is sequential in v1).
- `loop(s)` — repeat forever; `halt` — stop. (`when(c, s)` is a reserved constructor, not wired to program
  state — see "No runtime conditions" below.)
- `once(s) = seq({ s, halt })` and `forever(s) = loop(s)` are ordinary funcs. Higher combinators
  (`at_hz`, `every`, a fixed-step loop) are **not** in core or stdlib — the user writes their own as funcs
  returning `Schedule`. The core is just the five constructors above.

## Compile-time fold

`#run`'s argument CTFE-folds to a constant `ScheduleTree`; codegen walks it into one `@arche_run`
function — a bare leaf `s` → a direct `call @s()` (a `map` binds its pools first), `loop` → a branch
back-edge, `seq`/`par` → in order, `when` → a guard, `halt` → return. There is **no runtime `Schedule`
value and no function pointer**: the tree exists only at compile time, and `@main` calls `@arche_run` once.

`#run` is collected **only from the file you compile/run directly** — an imported module's `#run` is
**ignored** (a schedule belongs to the file you run, not a library). This is silent by design: no error,
no warning. So a file that is sometimes run and sometimes `#import`'d simply contributes no schedule when
imported; only the run-directly file's `#run` drives the program.

## There is no `World`

A `Schedule` is a pure description; effects happen only in the systems the runtime dispatches. The runtime
does **not** hand predicates a `World` value — the earlier `func(World) -> bool` design is gone, and
"World" is the ECS word for the pool universe anyway (which arche already has implicitly, as all the
pools). The state that a `World` would have bundled has concrete, separate homes instead:

- **clock** → a syscall (`clock_gettime`, wrapped as `os.now_ms`).
- **input** → a device writes an event pool; "is there input" is reading that pool.
- **tick count** → a `[1]` singleton a system bumps; just data.
- **core count** → deferred (only `par`-as-true-concurrency would want it).

## No runtime conditions

The schedule does **not** gate on program state. There is no `when`-over-state pass — the runtime reads no
pool or global to decide what runs; it walks the static tree and dispatches. The `when` constructor exists
in the core sum but is **not wired to program state**; its only plausible future use is **loop
termination** (stop the loop on a quit flag so teardown systems can run after it), which is a **deferred
TODO** — today a system calls `os.exit` to end the program. Pacing needs no condition (it's a system's own
work, below); dispatch needs none (the schedule just loops).

## Pacing: a system's accumulator, the schedule still looping dumbly

Timing is **work a system does**, not a schedule feature. A `pace` system reads the delta (`os.now_ms` vs
the `last` cell of a `[1]Clock` pool), accumulates it into a bank, and sleeps the remainder once caught up;
the sim `step` is its own scheduled kernel. Fixed-timestep **catch-up falls out of the schedule loop**: while
behind, `pace` doesn't sleep, so the loop spins and re-runs `step` next pass until caught up. No schedule
condition gates any of it.

```arche
DT :: 16;
[1]Clock(1);
Clock :: arche { last :: i64  bank :: i64 }        // clock state in a [1] pool, not a mutable global

step :: map (Movers) { … }                         // the sim tick — its own kernel, scheduled by name

pace :: system (query { last, bank }) {            // columnar over the [1]Clock singleton
  os.now_ms()(now:);
  bank = bank + (now - last);  last = now;
  if (bank >= DT) { bank = bank - DT; }            // a tick is owed → don't sleep; the loop re-runs `step`
  else { os.sleep_ms(i32(DT - bank)); }            // caught up → sleep the remainder
}

#run seq({ boot, forever(seq({ step, pace })) })   // schedule loops dumbly; `pace` does the pacing
```

A system **never dispatches** `step` — the **schedule** does, by name. **No language primitive is involved**:
the toolkit is the schedule core + `os.now_ms`/`os.sleep_ms`. (Draining *several* owed ticks in one frame
would need a runtime-conditioned loop — the deferred `when`/loop-end mechanism; today catch-up is one tick
per schedule pass.)

## State lives in pools

Concrete mutable state is a **pool** — a `[1]` singleton for one row (window handle, clock). The
mutable-global ban (W0022) fires on **any** mutable global, exported *or* `#file`-private: shared mutable
state belongs in a pool, never a global. There is no "driver" or "entry file" exemption, because running a
file is not a property of the file (the same file may be `#import`'d).
