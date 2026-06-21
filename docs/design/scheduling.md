# Scheduling

A program has no `main` and no hand-written driver loop. It is its declarations plus one `#run <Schedule>`
value; the runtime owns the loop. `Schedule` is a first-class value built from a small core (`core.arche`):

```arche
Schedule :: sum {
  run(system)  seq([]Schedule)  par([]Schedule)  loop(Schedule)
  when(func() -> bool, Schedule)  halt
}
```

- `run(s)` — dispatch a system `s`.
- `seq({…})` / `par({…})` — children in order (`par` is sequential in v1).
- `loop(s)` — repeat forever; `halt` — stop. (`when(c, s)` is a reserved constructor, not wired to program
  state — see "No runtime conditions" below.)
- `once(s) = seq({ s, halt })` and `forever(s) = loop(s)` are ordinary funcs. Higher combinators
  (`at_hz`, `every`, a fixed-step loop) are **not** in core or stdlib — the user writes their own as funcs
  returning `Schedule`. The core is just the six constructors above.

## Compile-time fold

`#run`'s argument CTFE-folds to a constant `ScheduleTree`; codegen walks it into one `@arche_run`
function — `run(s)` → a direct `call @s()`, `loop` → a branch back-edge, `seq`/`par` → in order, `when`
→ a guard, `halt` → return. There is **no runtime `Schedule` value and no function pointer**: the tree
exists only at compile time, and `@main` calls `@arche_run` once.

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

Timing is **work a system does**, not a schedule feature. The first system in the loop reads the delta
(`os.now_ms` vs a `last` driver global), accumulates it into a bank, drains owed ticks by running the sim
**maps** while behind, then sleeps the remainder once caught up. Fixed-timestep **catch-up falls out of the
schedule loop**: while behind, the system doesn't sleep, so the schedule re-runs it, draining one tick per
pass until caught up. No schedule condition gates any of it.

```arche
DT :: 16;
last : i64;  bank : i64;                           // the program's own clock state (driver globals)

frame :: system {                                  // the ONLY timing logic — all in one flat system
  os.now_ms()(now:);
  bank = bank + (now - last);  last = now;
  if (bank >= DT) {
    bank = bank - DT;
    run game.step;                                 // a tick owed → step the sim map, DON'T sleep; loop re-runs to catch up
  }
  else {
    paint(win);  gfx.poll(win)(open:);
    if (!open) { os.exit(0); }                     // loop end today: a system exits (no schedule condition)
    os.sleep_ms(i32(DT - bank));                   // caught up → sleep only the remainder
  }
}

#run seq({ run(boot), forever(run(frame)) })       // the schedule loops dumbly; frame does all the pacing
```

`game.step` is a **map** the system `run`s (a system can't dispatch systems, and there are no schedule
conditions to gate dispatch). **No language primitive is involved**: the toolkit is the schedule core +
`os.now_ms`/`os.sleep_ms`. The timing *choice* is the system's; the schedule only sequences and loops.

## State lives in pools or `#file`-private globals

Concrete mutable state is a pool (a `[1]` singleton for one row), or a `#file`-private mutable global.
arche-rpg holds its window handle and clock as `#file` globals (`#file` then `win : window`,
`last`/`bank : i64`), not `[1]Window`/`[1]Clock` singletons. The exported-mutable ban (W0022) is purely
**visibility-based**: it fires on any exported (no-banner) mutable global, in *any* file — there is no
"driver" or "entry file" exemption, because running a file is not a property of the file (the same file may
be `#import`'d). To hold private mutable state you mark it `#file`; it is then not exported and not flagged.
