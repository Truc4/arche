# Migrating arche-rpg to the flat effect model

### a concrete before/after for [the-flat-effect-model](the-flat-effect-model.md)

> **Status.** The model is a *design* (see [the-flat-effect-model.md](the-flat-effect-model.md)); the
> singleton-pool / device-system / `#run <Schedule>` forms here are not all implemented. The **"before"** is
> real current source from `~/Code/arche-rpg`. The **"after"** is illustrative. This is the demo for the
> **device-systems** discipline: a device exposes *systems over pools*, not callable procs — and that
> turns the one un-ECS seam in arche-rpg (a hand loop calling a draw proc) into a kernel.

---

## The program in one breath

A hot-reloadable toy: a driver (`rpg.arche`) owns the window and a `Player` pool of balls; a hot device
(`game.arche`) supplies spring physics and drawing. Each frame: fixed-step physics, clear, draw every
ball, present, poll for close. ~60 lines of hand-written loop.

## Before — the real source

The device's surface (`game/game.arche`):

```arche
Player :: arche { pos(x, y) :: int   vel(x, y) :: int   color :: int }   // game.arche:17

step :: map(Movers) {                          // game.arche:35 — physics: a SYSTEM (data-parallel)
  vel.x -= (pos.x - CENTER_X) / STIFFNESS;
  vel.y -= (pos.y - CENTER_Y) / STIFFNESS;
  vel *= DAMP_NUM;  vel /= DAMP_DEN;  pos += vel;
}

draw_ball :: proc(win: window, x: int, y: int, color: int) {   // game.arche:46 — a PROC, takes the window
  gfx.circle(win, x, y, BALL_RADIUS, color);
}
```

The driver loop (`rpg.arche:44-64`):

```arche
for (is_open := true; is_open;) {
  os.now_ms()(now:);  acc += (now - prev);  prev = now;          // timing
  if (acc > MAX_CATCHUP) { acc = MAX_CATCHUP; }
  for (; acc >= DT_MS;) { run game.step;  acc -= DT_MS; }        // fixed-step physics (system)

  gfx.clear(win, 0x101010);                                      // device proc
  for (i := 0; i < NBALLS; i += 1) {                             // ← the un-ECS seam
    game.draw_ball(win, Player.pos.x[i], Player.pos.y[i], Player.color[i]);   // device PROC, per entity
  }
  gfx.present(win);                                              // device proc
  gfx.poll(win)(is_open);                                        // device proc
  os.sleep_ms(2);
}
```

The tell: physics is a **system** that fans over the pool automatically (`run game.step`), but drawing is
a **proc** the driver calls in a hand-written per-entity loop — *because a system "can't take the window
handle."* So rendering can't be a kernel the way physics is. That asymmetry is the whole target.

## After — the window is a singleton; draw becomes a system; the loop is a `Schedule`

Make the window a `[1]Window` singleton pool. Now `draw` can be a **system** over `(pos, color)` that
reads `Window[0]` — and the per-entity loop disappears, exactly as it did for physics. There is no driver
loop to write at all: the program is its declarations plus one `#run <Schedule>` value, and the **runtime**
owns the loop:

```arche
[1]Window;   Window :: arche { handle :: window }     // external context as data (a singleton pool)

// the game device now exposes ONLY systems + pools + types — no draw_ball proc
draw :: system query { pos, color } {                 // fans over every ball, like step
  gfx.circle(Window.handle[0], pos.x, pos.y, BALL_RADIUS, color);   // reads the window singleton
}

// boot opens the window → insert Window{…}, seeds the Player pool; gfx.poll writes a should_close singleton

#run seq({                              // the program IS this one Schedule value; the runtime runs it
  run(boot),                            // setup runs once (it is in the seq); the loop follows
  forever(run(frame)),                  // the frame loop
})
```

The loop is just a value, `seq({ run(boot), forever(run(frame)) })`. **Pacing is not a language or stdlib
feature** — a fixed-timestep loop is game logic, so `frame` inlines its own accumulator using the OS clock
(`os.now_ms` / `os.sleep_ms`) and `last`/`bank` **driver globals**: add the elapsed delta to a bank, drain
owed ticks (`if (bank >= DT) { bank -= DT; run game.step }`, the schedule re-running `frame` to catch up if
behind), render once, then sleep the remainder. A `system` body may hold `for`/`if` and `run <map>`, which
is exactly what lets the loop be ordinary code. The schedule stays trivial; see [scheduling.md](scheduling.md).

## Where the model earns its keep here (the strengths)

1. **The hand loop dies; rendering becomes a kernel.** `draw` fans over the `Player` column exactly like
   `step`. Physics-and-render symmetry is the ECS ideal, and the device-systems rule is what reaches it —
   the proc existed *only* to thread the window, and a singleton dissolves that need.
2. **Every device effect is in the schedule.** Clear, draw, present, poll — none are imperative calls
   buried in a hand loop; they're scheduled systems (or runtime-woven lifecycle). Read the `#run` value,
   see the whole frame. The timeline is complete.
3. **External context generalizes cleanly.** Window today; `dt`, input state, camera tomorrow — all
   singleton pools systems read. The pattern that unlocks `draw` unlocks all of them.
4. **It fits hot-reload, which arche-rpg already lives on.** A device = systems + pools; reload swaps
   system bodies while pools persist. That's already how the project works; device-systems just makes the
   *interface* match the reality (the device was already mostly systems + a reluctant proc).
5. **Deterministic replay falls out of a dumb schedule + explicit time.** Wall-clock time enters the
   program at exactly one place — the pacing system's `os.now_ms` — not through a hidden runtime `World`
   (there is none). Feed that system a recorded `dt` instead of the real clock and the same systems, run
   by the same dumb `loop`, reproduce the run exactly. Same systems, a different time source. (The §6
   tape-test, for free.)

## Where it doesn't pay (the weaknesses — honest)

1. **Shared state a system reads must be a column, a singleton, or a `#file`-private global.** Per-entity
   data is columns; genuinely one-of state (the window handle, `last`/`bank` for the clock, a should-close
   flag) is a **`#file`-private mutable global** — `#file` narrows it off the exported surface, so W0022
   (which is purely visibility-based — it fires on any exported mutable global in any file, no entry-file
   exemption) does not apply. So the window is just `#file` then `win : window`, not a `[1]Window` pool +
   insert. The ceremony is small; the one rule is that a system reads *ambient* state, never as a parameter.
2. **The framebuffer borrow gets subtler inside a system.** `gfx.frame(win)` hands back a *writable view*
   the backend owns, invalidated by `present`. A `draw` *proc* grabs that view locally and is done; a
   `draw` *system* fanning per row has to respect the borrow/ownership rules (FFI slice, read-only borrow)
   across the kernel. Drawing-as-a-system is conceptually cleaner but mechanically fussier than the proc.
3. **Pacing stays application logic.** A fixed-timestep loop is game logic, not a language or stdlib
   concern, so there is no `every`/`catch_up` combinator — `frame` inlines its own accumulator over the OS
   clock (`os.now_ms`/`os.sleep_ms`). The language supplies only the primitives (systems, `run`, the
   schedule); the game composes them.
4. **Woven lifecycle is invisible in the source.** The runtime guaranteeing `present` at the frame seam is
   elegant, but for a one-file game an explicit `gfx.present(win)` is more legible than weaving the reader
   can't see in the `#run` value (the §9 "authored vs woven" wrinkle).

## Net

arche-rpg is the natural home for device-systems: drawing becomes a kernel symmetric with physics, the
manual per-entity loop vanishes, and the device's interface finally matches what it always was — systems
over a pool. The costs are that *all* external context must become singletons (ceremony a tiny driver
feels), the framebuffer borrow is fussier inside a kernel, and the prettiest parts depend on schedule
machinery that isn't built. Strong conceptual fit; for a program this small the payoff is mostly
cleanliness, not capability — the leverage would show in a real game with many systems and many entities.
