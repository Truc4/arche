# Flat Procedures — Systems, Effects, and Data-Oriented Composition

> **Status: design exploration — direction now chosen.** The load-bearing decisions are
> settled: keep **systems**, keep the **proc→proc ban**, and pass effects as **bounded
> data descriptors** built by pure funcs. The earlier *algebraic-effects* machinery —
> `perform`/`resume`/`handler`, dynamic install-stacks, effect hoisting — is **dropped**;
> it bought nothing the descriptor model doesn't, and worked against the goals (flat +
> explicit). This records the thesis, the resulting language shape, the open forks, and
> how *real* code in arche / arche-web-server / arche-rpg would migrate.

---

## 1. Context & motivation

Today an arche `proc` may call another `proc`. That builds a runtime call tree, and
the two objections to it are distinct:

- **Pragmatic.** Proc→proc nesting grows the runtime stack to an arbitrary,
  not-statically-known depth. For a language with **no dynamic allocation** and a
  realtime/embedded ethos, a stack bound you can't compute at compile time is a real
  liability.
- **Conceptual.** *Procedural code should mean a flat sequence of steps, not chasing
  references all over a call graph.* Deep nesting and "chasing stuff everywhere" is
  what the **functional** world is for — pure functions compose and recurse safely
  because they're referentially transparent and always terminate back into a value.

The core move: **ban an effectful unit from calling or running another** — the headline
case is `proc`→`proc`, and once §3 splits effectful work into call-once `proc` and
fanned `each`, the rule covers every leaf→leaf. Effectful units become flat leaves;
composition of effects moves to a dedicated layer (`system`). The functional world
(`func`) is untouched — that's where nesting and recursion belong.

This is not exotic. It's the discipline of **synchronous dataflow languages**
(Lustre, Esterel, Signal — realtime/embedded, statically bounded, no heap),
**flow-based programming**, **ECS** system scheduling, and **hardware** module
composition (Verilog modules are *instantiated and wired*, never "called"). arche
already leans this way: a `map` is a flat, call-free, no-control-flow kernel driven
by `run`.

---

## 2. The thesis in one picture

| Kind       | World / shape                  | Calls / nests?                                  | Runtime stack |
| ---------- | ------------------------------ | ----------------------------------------------- | ------------- |
| `func`     | pure value — call-once         | nests & recurses freely; **builds descriptors** | unbounded (the one opt-in overflow source — §7) |
| `proc`     | effect — call-once `(in)(out)` | funcs + intrinsics/externs; executes descriptors; never calls a proc/each | depth 1 |
| `each`     | effect — fanned over a pool    | funcs + intrinsics/externs; executes descriptors; never calls a proc/each | n/a (runtime owns the loop) |
| `map`      | pure — fanned over a pool      | no calls; branch-free (`select`); builds descriptors | n/a (runtime owns the loop) |
| `system`   | composer                       | runs procs, `run`s each/map, drains descriptors via executor procs | — |
| `schedule` | top-level                      | registers systems                               | — |

**Effect call-depth is exactly 1** — system → leaf (`proc`/`each`), or system → executor
`proc`. All remaining depth lives in the pure functional world. The ban makes the
*effectful* stack a compile-time constant — the pragmatic win, developed in full in §7
(a *certified maximum stack size*) — and forces procedural code flat — the conceptual win.

**The outside world is an effect, and foreign code is *always* an effect.** Touching the
world or calling foreign C is never a free inline call — it is reached by *building a
descriptor* and *executing* it at the boundary (where the real syscall / C-call lives).
**There is no `extern func`:** foreign code is always `extern proc`, i.e. effectful. That
keeps C from being the least-friction way to write logic — the friction sits squarely on
*doing*, by design.

So the leaf-callable **floor** is just: native `func`s and a *closed, language-blessed
set* of **intrinsics** — pure value primitives (`select`, arithmetic, slicing).
Everything that *does* something is an effect: I/O, all foreign code, structural pool
changes (`insert`/`delete`, applied at a schedule barrier), and byte production
(formatting writes memory, so it's an effect too — §5). The line is the **contract, not
the implementation**: an op is an effect iff its behavior depends on something beyond its
arguments — the world or shared state; a total single-meaning function of its arguments
is pure.

A `func` still participates in effects *without becoming impure*: **a func builds a
first-class effect *descriptor*** — a bounded data value (op tag + bounded args) — and
returns it like any other value. A `proc`/`each` later *executes* it (§5). Pure code
builds the plan; an executor (a proc the system runs, or a runtime executor) does the
work. Systems compute nothing; they order steps and drain descriptors. The C frame's
stack cost rides on its executor proc (§7).

---

## 3. The kinds, carved by I/O shape

A key realization: **out-params are a call-*once* concept.** A caller allocates a
slot; the callee writes it once. Fan that over N rows and a single out-slot is
meaningless (last-write-wins). So units split into two worlds by their I/O *shape*:

### Call-once — caller passes args, gets slots back

```arche
area   :: func (w: int, h: int) -> int             // pure value, one return
divmod :: proc (a: int, b: int)(q: int, r: int)    // effectful action, out-slots, runs once
load   :: proc (path: []char)(count: int)
```

### Fan-out — runtime owns the loop, binds per-row; **no out-params**

The query is a *separate axis* from runtime params, and should read that way — it is
**not a runtime argument**. It's the per-row, compile-time iteration space (the
columns), bound from the pool. Runtime `(in)` params are **uniforms** — one value,
constant across the whole fan-out. (This is exactly the GPU distinction between a
per-thread attribute and a uniform.)

Proposed shape — query lifted out of the param parens, right after the kind keyword:

```arche
name :: <kind> query { per-row columns } ( uniforms )
```

```arche
integrate  :: map  query { pos, vel } (dt: float)  // per-row cols + uniform; writes columns
dampen     :: map  query { vel, pos }              // branch-free; select() for conditionals
render_row :: each query { id }                    // per-row; writes columns + executes effects
```

A fan-out's *output* is the **per-row column write** (in place) and/or **effects** —
never a scalar slot. When you need *one scalar out of many rows*, that is not an
out-param either; it is a **reduce/scan** (which already exists in the ParOp IR as
`PAR_REDUCE` / `PAR_SCAN`):

```arche
total :: reduce query { revenue } -> float (+)     // fold N rows → 1 value, with a monoid
```

**Presence of `query` encodes execution mode:** a signature with `query {}` is fanned
by `run` over a pool; one without is a one-shot. The query-bearing kinds are `map` and
`each` (fanned, `run`); `func` and `proc` never carry one (call-once).

### The effectful fan-out: `each`

The effectful fan-out is **`each`** — the sibling of `map`: same shape
(`each query{}(uniforms)`, writes columns, invoked by `run`), differing only in that
`map` is pure/branch-free/vectorized while `each` is effectful/branchy/sequential. `map`
stays the word that *guarantees* "this provably vectorizes / lowers to GPU" (enforced by
E0046); `each` is where per-row effects and branches live. That completes the clean 2×2
of `{pure, effectful} × {call-once, fanned}`: `func`/`proc` (call-once) and `map`/`each`
(fanned). `each` has **no out-params** — its output is column writes, executed effects,
or a `reduce`; out-params belong to `proc` (call-once) only.

*(Considered, not taken: a single fanned kind whose vectorizability is inferred and
`@gpu`-asserted — fewer keywords, but `map`'s GPU guarantee would become a checked
property rather than a visible kind. Kept the distinct-kind form for the contract.)*

---

## 4. Systems & the schedule (composition)

The old `main` — an orchestrating proc that *calls* other procs — disappears; its two
jobs split:

- **Wiring** (thread one step's result into the next) → a `system` (plural, named).
- **Control of *when* things run** (the loop) → the `schedule` (singular).

The `schedule` is **not a value you bind** — there is exactly **one per driver**, and
its presence is what *makes* a file the executable's entry point (it replaces `main`).
A library or a reloadable *device* has zero: it exposes systems/maps/procs to be driven,
it doesn't own the loop. (This maps onto arche's existing driver/device split — the
driver owns the schedule; the device is driven.) So it reads best as a **singular
top-level directive** in the family of `#import` / `#foreign` / `#file` — not a
`name :: schedule` binding, which would wrongly imply you could name, duplicate, or pass
it:

```arche
#schedule {
  once boot;        // startup systems
  loop frame;       // per-tick systems (runtime owns the loop)
}
```

The compiler enforces exactly one `#schedule` in a driver and rejects a second (or any
in an imported library/device). Contrast with `system`, which is plural and named
precisely because you compose many — **the binding form encodes the cardinality.**
*(Open, cosmetic: the `#` of a directive vs. a reserved `schedule {}` block; the
load-bearing facts are singular, not-a-value, and entry-marking.)*

A **system is pure glue**: it `run`s procs/maps/each, threads one step's out-arg into the
next step's in-arg, and **drains descriptors by running an executor proc**. **No `for`,
no `if`.** Iteration is *always* `run` — the runtime owns the loop (arche already has no
`for … in`; collection iteration is `run` + map). Counted loops with no data live
*inside* a proc as C-style `for`. Branching for *which data* lives in queries (see §6),
not in the glue.

```arche
report :: system {
  run render_rows;            // fan the effectful leaf over the pool → append Out descriptors
  drain_to_file("out.txt");   // run an executor proc that drains the Out descriptors + writes
}
```

`run mapname` → pure kernel, vectorized. Running an `each` → effectful, sequential.
Effects are either executed directly at the leaf (the leaf calls an intrinsic/extern), or
emitted as descriptors and drained by an **executor proc the system runs** — a system
*may* run a proc (system→proc is fine; only proc→proc is banned). Uniforms are bound at
the run site: `run integrate(dt);`. Write **one `run` per line** — never several stacked
on a line; a row of `run a; run b; run c;` reads as one confusing statement, so each gets
its own line.

**Choosing the executor is how a system swaps behavior.** Which executor proc a system
runs (live vs. recorded, screen vs. file) is the system's choice, bound **statically** —
by name, or by a compile-time callback (arche already monomorphizes proc/func-typed
params). There is no dynamic lookup and no install-stack; the binding is visible at the
`system`.

**Params, and how systems touch them.** Params are very much still in use: a fanned
`map`/`each` takes **uniforms** in `(…)` bound at the run site, and a **call-once**
`proc` keeps its full `(in)(out)`. A `system` *runs* call-once procs and **threads**
one step's out-arg into the next step's in-arg — the wiring the old `main` used to do:

```arche
tick :: system {
  read_clock()(dt:);     // call-once proc → out-arg `dt` enters system scope
  run integrate(dt);     // forward `dt` as a uniform to the fanned map
  run render;
}
```

That threading is the system's whole relationship to params: it **binds and forwards**
(out→in, plus literals), it never **computes**. `run integrate(dt * 0.5)` is illegal —
arithmetic is logic; compute it in a proc/func and thread the result. So a system
interacts with params by *wiring*, not by *calculating*.

### The simple case: an implied schedule

You rarely write `#schedule` for a one-shot program. If a driver defines a `main` and
has **no** explicit `#schedule`, the compiler implies:

```arche
#schedule { once main; }
```

So a script-like program is just `main :: system { … }`, run once — the familiar entry
point, no ceremony. `loop`, phases, and multiple systems are exactly when you reach for
an explicit `#schedule`. The rule composes cleanly with "one schedule per driver":

- explicit `#schedule { … }` present → that is the entry (and a `main`, if any, is just
  an ordinary system you may reference like any other — no special meaning);
- else a `main` present → implied `#schedule { once main; }`;
- else → no entry: the file is a library / reloadable device (driven, not a driver).

This is **sugar, not a special proc.** The orchestrating-`main`-that-calls-procs is
still gone; this `main` is simply the system the implied schedule runs once, with no
built-in semantics of its own. It is normally a `system` (so it can `run` + compose); a
bare `main :: proc()()` is allowed for the trivial case — it works only because the
effects it executes (e.g. printing) are the runtime's *ambient* primitives (§5), and the
moment it needs to run an executor or sequence steps it becomes a `main :: system { … }`.

---

## 5. Effect descriptors (effects as data, no proc→proc)

With proc→proc banned, the thing a proc-call used to do — *invoke a named action with
args* — is replaced by **building a descriptor**. A descriptor is a **bounded data
value** that *describes* an effect: an op tag plus a fixed-size argument payload. It is
inert — it does nothing until something **executes** it.

**funcs build descriptors — this is the keystone.** A `func` is pure and may be called,
nested, and reused freely (the ban never touches it). If a func can build effect
*descriptions as data*, then **all reusable logic** — pure compute *and* "what effect to
do" — lives in the func world, and the proc world shrinks to a thin executor. This is
what makes the proc→proc ban survivable: you don't factor effectful behavior into a
sub-proc (banned), you factor the *descriptor-building* into a func (free).

```arche
// pure, reusable: builds a bounded descriptor — renders nothing, touches nothing
line :: func(price: float, quantity: int, bucket: float) -> Line {
  return Line { price, quantity, bucket };   // a fixed-size value, not bytes
}
```

**Executing a descriptor — local, never hoisted.** A descriptor is executed in one of
two places, both visible and static:

- **At the leaf.** A `proc`/`each` calls an intrinsic/extern directly — `os.write`,
  `gfx.draw` — turning the descriptor (or its own arguments) into a real effect *now*.
- **By an executor proc the system runs.** A leaf *emits* descriptors into a static
  command pool; a `system` then runs an **executor proc** that drains the pool and does
  the work. `emit` is the general form of the `insert`/`delete` the runtime already
  defers (Bevy "Commands"); the executor is an ordinary `proc`/`each` the system runs.

There is no `perform`, no `resume`, no `handler`, no dynamic install-stack, and no effect
that "jumps out" to be caught somewhere up the call chain. **This is not algebraic
effects** — it's plain data plus a statically-chosen executor (closer to a command buffer
/ compile-time DI). That staticness is exactly what §7's certified stack bound needs.

**Inferred effect set.** Because procs can't call procs, a proc's effect set is *whatever
it textually executes or emits, and nothing more* — local, shallow, no fixpoint over a
call graph. The compiler **infers** it and the tooling *displays* it; `uses {Net, Fail}`
stays available as an optional checked contract on public signatures. A `func`'s inferred
set must be empty — *except* the pure, compile-time, tail-resumptive failure effect arche
already ships: the **policy system** (`!clamp`/`!zero`/`!abort`), a `Fail` resolved at
the op site, which keeps funcs GPU-safe.

**Checked at the system boundary.** Every descriptor kind emitted in a system's extent
must either have a runtime executor (below) or be drained by an executor proc the system
runs — otherwise it's a compile error ("`Out` emitted but never drained").

### Naming: a name claims no more than its layer commits

Effect ops are named for their **role/intent, never their mechanism.** The leaf that
builds or emits a descriptor knows *what it wants done*, not *how* or *where* — so the
name must not smuggle in a commitment the leaf doesn't make.

- **Role/sink descriptors → intent-names.** `emit`, `yield`, `log`, `render` — the leaf
  asks for an outcome; the executor the system runs decides the sink. An `Out` descriptor
  (not `WriteFile`): the leaf emits a row, it does not know a file exists.
- **Resource descriptors → mechanism-names.** `File.read` / `File.write`,
  `Net.send` / `Net.recv` — here the *resource* is the whole point, so naming the
  mechanism is honest; the op is already committed to a kind of resource.

The general rule: **a name must not claim more than its layer commits to.** A leaf
doesn't name a destination; the commitment lives at the `system` that picks the executor.
This is why §8.6's shared leaves read `draw`/`emit` and never `blit`/`write_png`: those
mechanisms belong to the executor the system swaps, not to the reusable leaf.

### Runtime executors & the OS floor

Not every effect needs a user-written executor. The **primitive effects** — `File`,
`Net`, `Time`, randomness, `Fmt` (byte rendering), and structural `insert`/`delete` —
have **runtime executors** (ambient), so a leaf may execute them with no executor proc in
scope. More broadly, **much of the stdlib (`fmt`, `io`, `net`) is descriptors + the
funcs that build them**: the pure surface is `func`s that produce descriptors + data
types, and the runtime executor does the rendering and syscalls (§8.2). *User-defined*
effects (e.g. an `Out` that writes formatted lines to one chosen file) need a `system` to
run an executor proc.

The actual OS boundary is `os.*` (`os.read` / `os.write` / `os.open` / `os.close` /
`os.syscall`) — the single layer that issues a real syscall. These are callable only from
the **impure leaves** (`proc`/`each`, including executor procs), never from a `func`/`map`
— that's just the pure/impure split. A leaf builds or emits a high-level descriptor; the
executor (runtime-provided or a proc the system runs) bottoms out in `os.*`. This is what
§2's "the C-call lives at the boundary" cashes out to.

### Structural effects (`insert` / `delete`)

`insert`, `delete`, and live-count changes are the effects that change a pool's *shape*
— which rows exist — rather than a row's *values*. They are the original descriptor /
command-buffer in arche, handled specially because of an ECS hazard: **you cannot
structurally mutate a pool you're currently iterating** (a `run`/`map` over it) — that
invalidates the iteration, and races if stages run in parallel on the cores backend. So:

- **Emitted in a leaf, applied at a barrier.** A leaf *emits* the structural change
  (per-row logic decides what to spawn/remove); it is **queued**, and the runtime
  **flushes it at the schedule/system seam** (between `run`s). New rows become visible to
  the *next* stage. This is exactly Bevy's deferred "Commands."
- **Immediate vs. deferred is by context, and statically decided.** *Immediate* when the
  leaf is **not** fanning the target pool (e.g. the router building its trie in a `once`
  system — works as today, and `insert` returns the slot synchronously). *Deferred* when
  performed **inside a `run` over that pool**. The compiler knows which case applies.
- **The executor is the runtime.** No user executor is written — the runtime knows how to
  add/remove a pool row. These are descriptors with a *built-in* executor, like the core
  I/O effects above.
- **Overflow is a policy.** A static pool can fill; `insert` then follows the pool's
  `overflow_policy` (which arche already has) — the same in-leaf failure mechanism as
  everything else. The command pool is itself a static, per-tick-bounded buffer (no heap)
  with its own overflow policy.

**The synchronous-slot tension.** A *deferred* `insert` cannot hand back a usable row
index — the row does not exist until the flush. The router's "insert → get slot → link
it in immediately" pattern relies on the *immediate* case (it runs in setup, not fanning
the pool). If you ever need spawn-and-immediately-reference *mid-fan-out*, you need
reserved/promised ids (Bevy does this) or you simply don't get the index until after the
barrier. §8.4's `insert Conn rows` / `delete done rows` depend on these deferred-at-barrier
semantics: spawns land for the next stage, deletes don't disturb the current fan-out.

### Descriptors are not monads

A descriptor is plain bounded data — a tagged value, nothing more. There is no `bind`, no
monadic chaining, no transformer stack, and no closure or continuation capture. So the
`IO a` / free-monad framing — and the heap and HKT baggage it drags in — simply does not
apply here: a func returns *a* descriptor as a value, a leaf executes it, and that's the
whole story.

---

## 6. Conditionals & data-oriented selection

The data-oriented discipline: **don't iterate-then-test; query the set so the data you
touch *is* the condition.** A condition is either:

- **structural** — expressed by *which pool/query* (decided by data layout), or
- **produced as data** — a mask/tag column computed by a (branch-free) map.

`if` never decides membership or whether-to-process a row. It survives only inside a
leaf reasoning about its own row's internals — and the goal is to make even that rare.

- **Pure work:** the condition stays in-kernel as a branch-free mask — already the
  `dampen` pattern: `vel = vel * (pos > 10);` / `select(pos > 100.0, 0.0, vel)`.
- **Effectful work:** you can't `select` away a side effect, so the matching rows must
  be a real set *before* the run — **compact/partition them contiguous** (the
  permute + live-count machinery that already backs sort), then `run` over that range.
  Selection becomes a data-reorganization step, not a per-row branch.

### GPU-safety, stated correctly

Branches are **not** what makes code GPU-unsafe. A GPU is SIMT: branches execute via
predication / an execution mask (the compiler if-converts small branches; divergence
is a *performance* cost, not a correctness one). The real bars are **no effects** and
**static bounds**. So: a **`map`** is the *guaranteed* GPU citizen because it's
branch-free *by rule* (E0046 → `select`), upgrading "correct on GPU" to "divergence-
free on GPU." A **`func`** has branches and is still *purely lowerable* (predication),
just not divergence-free. That gap is exactly why `map` bans branches and `func`
keeps them.

---

## 7. Stack discipline & the certified bound

**What.** The compiler computes a single worst-case stack size for the entire
arche-controlled program and guarantees it is never exceeded. Stack overflow stops
being a runtime crash and becomes a **compile-time error** ("needs 12 KB, budget is
8 KB") — the same move bounds-checking makes for arrays.

**Why.** The no-dynamic-allocation rule already pins the *heap* footprint at compile
time (every pool is a static capacity). A certified stack bound pins the *other* half.
Together: **total static memory footprint** — pools + stack, every byte known before
the program runs, provably fits in a fixed SRAM budget, no `malloc`, no overflow. That
is the property safety-critical / embedded work demands (DO-178C, MISRA), and today
it's achieved *externally* — a stack analyzer (AbsInt, GCC `-fstack-usage`) over a call
graph, sound only because a coding standard banned recursion and indirect calls. Most
general-purpose languages can't promise it at all: recursion + dynamic dispatch make
the call graph unknowable. This design makes the call graph statically known **by
language rule**, so the guarantee holds by construction rather than by external audit.

**How — the formula.** With the proc→proc ban, every effectful path is flat or
iterative (`run` over a pool is the runtime's loop; each step returns before the next),
and an executor proc is just another proc the system runs (depth 1), so:

```
max_stack  =  spine(schedule + system frames)
           +  max(proc frame size  over all procs)   // incl. executor procs — each depth-1 from a system
```

Every term is a compile-time constant. Descriptors don't add a term: they are bounded
data living in pool columns / a static command pool, not stack frames, and executing one
is a single (depth-1) proc/each.

**How — the conditions** (each closes an otherwise-unbounded source of depth):

1. **No proc→proc / no proc recursion** — the core ban. Removes effect-spine recursion.
2. **Statically resolved calls/executors** — no dynamic dispatch, no runtime proc-values;
   executors are bound by name / compile-time callback (§4). This is the *same* decision
   that gives the flat stack (§5); dynamic dispatch would reintroduce an unknown graph.
   One choice, both payoffs.
3. **Funcs carry no unbounded recursion** — funcs are the one place the design opts
   *back into* depth (the pure compositional world). A non-recursive func is fine (its
   call graph is also a DAG, still summed). A *recursive* func must carry a static depth
   bound (`@depth(≤N)`) to stay in budget, or it is excluded from the guarantee. So the
   real rule is **"no unbounded recursion anywhere,"** not "no funcs."

**How — bound vs. fit (two steps).** Conditions 1–3 make `max_stack` *finite and
computable* — but finite ≠ fits: fat frames (big `[K]char` locals) could still exceed the
physical stack. So the guarantee is two steps:

1. compute `max_stack` (possible *because* the graph is statically resolved + acyclic);
2. **verify `max_stack` ≤ the declared budget, and reject the program otherwise.**

Step 2 is what turns "computable" into "impossible" — by refusing to emit a binary that
wouldn't fit. The knob:

- `arche build --max-stack=8K` — a whole-program ceiling; compile error if exceeded.
- `@stack(≤512)` on a proc or system — a checked, *composable* contract (a scheduler
  sums its leaves' declared bounds), verified like a type.

**Edges.**

- **FFI cost rides on its executor proc.** Foreign code is reached through an executor
  (§2), so the C frame is part of *that proc's* budget, declared via its `@stack`. An
  extern that recurses unboundedly on the C side is still outside arche's analysis, so
  it carries a declared budget or the bound is scoped: "arche-controlled stack ≤ N,
  plus trusted C."
- **It's a ceiling, not magic.** A big-but-bounded program is *rejected at compile
  time*, not silently shrunk — that rejection is the feature.

---

## 8. Migration of real code

### 8.1 ETL pipeline — the clean win

`design_analysis/benchmarks/etl/1k/single-thread/arche/task_3_bucket_timestamps.arche`
is the canonical read → transform → write. **Today:**

```arche
load_transactions :: proc() {
  csv.load(Transaction, "…/data_1k.csv");                 // proc→proc
}
write_transactions :: proc() {
  io.fopen_write("…/arche_output.csv")(fd:);
  for (i := 0; i < 1000; i = i + 1) {
    buf: [256]char;
    fmt.sprintf(_, "%f,%d,%f\n",
      Transaction.price[i], Transaction.quantity[i], Transaction.price_bucket[i])(buf, n:);
    io.fwrite(fd, move buf, n);                            // proc→proc
  }
  io.fclose(move fd);
}
main :: proc() {
  load_transactions();
  Transaction.price_bucket = Transaction.price / 10.0;     // already a column op
  write_transactions();
}
```

**After:** the transform is *already columnar* → a `map`. A pure `func` builds the row
descriptor (reusable); a fanned `each` executes it (render + write at the leaf via the
`Fmt`/`File` runtime executors); `main` dissolves into the `#schedule`.

```arche
bucket :: map query { price, price_bucket } { price_bucket = price / 10.0; }

// pure, reusable: builds the bounded line descriptor (spec + args), renders nothing
fmt_row :: func(price: float, quantity: int, bucket: float) -> Line {
  return line("%f,%d,%f\n", price, quantity, bucket);
}

// executor each: fan over Transaction, render each descriptor → bytes, write — fd is a uniform
write_csv :: each query { price, quantity, price_bucket } (fd: fd) {
  os.write(fd, render(fmt_row(price, quantity, price_bucket)));   // render (Fmt) + write (File)
}

open_csv  :: proc(path: []char)(fd: fd) { os.open(path, WRITE)(fd); }
close_csv :: proc(fd: fd)()             { os.close(fd); }

write_out :: system {
  open_csv("…/arche_output.csv")(fd:);   // one-shot proc opens
  run write_csv(fd);                      // fan: build descriptor (func) + execute (each)
  close_csv(fd);                          // one-shot proc closes
}

#schedule {
  once load_tx;     // read + parse → fills Transaction (File runtime executor)
  once bucket;      // compute (map: vectorizes / cores)
  once write_out;   // build descriptors (func) + execute (each)
}
```

### 8.2 The IO wrapper towers — collapse to one hop + funcs

`stdlib/io/io.arche` is a tower of proc-over-proc-over-extern — and several wrappers
already carry `@allow(proc_could_be_func)`, the language *noticing* this smell. The
ban forbids the middle arche-proc layers:

```arche
read :: proc(f: fd, buf: []char)(out: []char) {
  fread(f, buf, buf.length)(r:);   // arche-proc → arche-proc → extern: BANNED
  out = r;
}
open       :: proc(own path: []char)(f: fd) { fopen_read(move path)(f); }      // BANNED
skip_header:: proc(f: fd) { line: [256]char; fread_line(f, move line, 256)(_discard:); } // BANNED
```

**After:** there is no `read`→`fread`→`syscall` tower because the impure leaf reaches the
syscall in **one hop**, and every reusable piece of *logic* is a `func`:

```arche
load_tx :: proc()() {
  os.read(fd, buf)(chunk:);        // one hop: leaf → syscall extern (File runtime executor)
  parse_line(chunk)(record:);      // parse_line is a FUNC — pure, reusable, nests freely
}
```

So the `@allow(proc_could_be_func)` wrappers that flagged the smell don't exist in this
model — there is nothing to wrap. The world is touched once, at the leaf; the reuse lives
in funcs (and, where output must be staged, in descriptors). (The earlier "DRY dies in
the wrapper tier" worry was an artifact of treating FFI as a free inline call; once I/O is
a one-hop effect with a runtime executor, the wrapper towers are gone, not duplicated.)

### 8.3 The router — recursion-free already; thin wrappers inline

`stdlib/router/router.arche`'s `add` is iterative (a segment loop), not recursive — so
arche already avoids effectful recursion here. The only proc→proc is `new_node`, a
thin wrapper over `insert` + an `i32` cast:

```arche
new_node :: proc(seg: []char, seg_len: int, kind: nodeKind)(idx: int, ok: int) {
  insert(TrieNode { … })(slot:, ok);   // builtin, not an arche proc
  idx = i32(slot);
}
```

`add` calls `new_node` → inline it (it's a wrapper). `resolve` returns a route id
(data, not a call) and stays as-is. The router migrates with near-zero structural
change — it was already data-oriented.

### 8.4 HTTP request lifecycle — dispatch-as-data + the staged pipeline

`stdlib/http/http.arche`: the dispatch branches to a *different proc per arm* —
proc→proc chosen by data, the hardest case:

```arche
match h {
  route.home      : respond(conn, hdr, 200, "text/html", home_html, true);
  route.user      : respond(conn, hdr, 200, "text/html", user_html, true);
  route.not_found : respond_text(conn, hdr, 404, "Not Found\n");
}
respond_text :: proc(conn, hdr, status, msg) { respond(conn, hdr, status, "text/plain…", msg, true); } // proc→proc
```

**Resolution: make the branch choose *data*, not a procedure.** There is no `respond`
proc to call (that would be proc→proc). The branch writes *data* to the row's columns,
and a single later stage sends it uniformly — arche already cut this seam: `resolve`
returns an id, it never calls a proc:

```arche
// inside serve_step — the branch picks DATA, never a proc:
status := 200;  body := home_html;
body   = select(route_id == route.user, user_html, body);
// route.not_found → status = 404, body = "Not Found\n", …
// these land in the row's status/outbuf columns; the actual send is send_step (Net.send)
```

**The payoff — connections as a pool, the lifecycle as a staged pipeline.** Once
dispatch is data, the deep per-request *call stack* becomes a wide per-connection
*data pipeline* (how high-throughput servers are actually built). Every transient that
lived on the stack becomes a `Conn` column:

```arche
Conn :: arche {
  fd :: int, stage :: int, nread :: int, route_id :: int, status :: int,
  inbuf :: [1024]char, outbuf :: [1024]char,        // array columns (cf. TrieNode.seg, Message.text)
}
[256]Conn(0);                                        // 256 live conns, static, no heap

accept_new :: proc()()                          { /* os.accept; insert Conn rows (deferred — flush at barrier) */ }
recv_step  :: each query { fd, inbuf, nread }   { /* os.recv → inbuf (Net runtime executor) */ }
parse_step :: each query { inbuf /*, method/target cols */ }  { /* fill columns (pure intrinsics/funcs) */ }
route_step :: each query { /* target →  */ route_id }         { /* resolve → route_id (data) */ }
serve_step :: each query { route_id, status, outbuf }         { /* branch picks DATA → fill status/outbuf */ }
send_step  :: each query { fd, outbuf, status }               { /* os.send; emit delete on done rows */ }

serve :: system {                  // Net has a runtime executor — no executor proc to write
  accept_new();                    // one-shot; its inserts flush before recv_step runs
  run recv_step;
  run parse_step;
  run route_step;
  run serve_step;
  run send_step;
}
#schedule { loop serve; }
```

Stages over disjoint columns are exactly what the **cores backend** can parallelize.

### 8.5 RPG main loop — input/sim/render → schedule + run

`arche-rpg/rpg.arche` is a hand-written fixed-timestep loop with nested `for`s, a
`run game.step`, and a per-ball draw call:

```arche
main :: proc() {
  gfx.open(…)(win:);
  Player.pos.x = {…};  /* seed columns */
  acc: i64 = 0;  os.now_ms()(prev:);
  for (is_open := true; is_open;) {
    os.now_ms()(now:);  acc = acc + (now - prev);  prev = now;
    if (acc > MAX_CATCHUP) { acc = MAX_CATCHUP; }
    for (; acc >= DT_MS;) { run game.step; acc = acc - DT_MS; }   // sim
    gfx.clear(win, …);
    for (i := 0; i < NBALLS; i += 1) {
      game.draw_ball(win, Player.pos.x[i], Player.pos.y[i], Player.color[i]);  // per-ball
    }
    gfx.present(win);  gfx.poll(win)(is_open);  os.sleep_ms(2);
  }
}
```

**After:** the loop becomes `#schedule { loop … }`. `game.step` is already a `map`.
The per-ball draw becomes a fanned `each` (`run draw_ball`) over the `Player` pool that
executes a draw at the leaf; `win` is opened once by a boot proc and threaded as a
uniform. Clear/present are one-shot procs the system runs. The fixed-timestep accumulator
is the one genuinely tricky bit (control logic about *when* to run sim vs. draw) — it
likely needs schedule-level support for fixed-rate vs. per-frame phases, or stays inside
a small one-shot proc.

```arche
draw_ball   :: each query { pos, color } (win: window) { gfx.draw(win, pos.x, pos.y, color); }  // execute at leaf
clear_win   :: proc(win: window)()                     { gfx.clear(win, 0x101010); }
present_win :: proc(win: window)()                     { gfx.present(win); }

frame :: system {
  run game.step;             // sim (a map)
  clear_win(win);            // one-shot
  run draw_ball(win);        // fan over Player; each executes gfx.draw
  present_win(win);          // one-shot
}
#schedule {
  once seed_world;           // opens the window (singleton), seeds columns
  loop frame;                // fixed-timestep policy: open Q (§9)
}
```

### 8.6 Reuse: one set of leaves → three programs

The payoff of migrating to leaves + descriptors: one tiny set of building blocks becomes
a game, a renderer, and a test. `play`/`export` differ only in *which edge stages the
system runs*; `verify` differs only in *composition*. The middle logic never changes.

**Shared building blocks** — written once, knowing nothing about keyboards, windows, or
files (names commit to *intent*, never mechanism — §5). Input is a **column** filled by a
producer stage; output is a draw executed at a consumer stage:

```arche
control   :: each query { vel, dir }   { vel = dir * SPEED; }   // `dir` is a column a producer filled
integrate :: map  query { pos, vel }   { pos = pos + vel; }     // pure physics
```

**Edge stages** — the swappable mechanisms, each an ordinary `proc`/`each`:

```arche
// producers: fill the `dir` column (input as data, not a mid-computation request)
keys :: each query { dir }            { dir = os.poll_key(); }       // live keyboard
tape :: each query { dir } (fd: fd)   { dir = read_dir(fd); }        // recorded commands

// consumers: execute a frame draw to a chosen sink
screen :: each query { pos, color } (win: window) { gfx.blit(win, pos.x, pos.y, color); }   // to a window
png    :: each query { pos, color } (fb: frame)   { fb_put(fb, pos.x, pos.y, color); }       // to a framebuffer
```

**Two systems, two programs — identical middle, swapped edges:**

```arche
play :: system {                   // interactive game on screen
  run keys;                        //  input from the keyboard
  run control;
  run integrate;
  run screen(win);                 //  frames to a window
}

export :: system {                 // deterministically re-render a recording into image files
  run tape(fd);                    //  input from a recording
  run control;
  run integrate;
  run png(fb);                     //  frames to a framebuffer
  write_png("frames/", fb);        //  one-shot: flush the framebuffer to disk
}
```

`control`/`integrate` are byte-for-byte the same in both. `play` is a keyboard-driven
game; `export` turns a recorded session into PNGs — deterministic, headless. The middle
leaves never learned what a keyboard or window is; the systems chose, by which producer
and consumer stages they ran. *(One parametric system with compile-time-callback edge
stages — `world(input, output)` — collapses the two into one, if you want it.)*

**A third program — same pieces, *recomposed*:**

```arche
verify :: system {                 // deterministic physics test — no rendering at all
  run tape(fd);
  run control;
  run integrate;                   //  no consumer stage is composed in
  check_invariants();              //  one-shot test leaf asserts the final positions
}
```

`verify` reuses `control` + `integrate`, drops the consumer, adds an assertion. One set of
leaves → **game, renderer, test suite**, with zero changes to the logic. The line a
programmer feels first: **you test the real game logic with no mocking framework — you run
a `tape` producer stage instead of `keys`.**

---

## 9. Open questions

1. **(Resolved) The effectful fan-out is `each`** — its own kind, distinct from the
   call-once `proc` (§3). Alternative not taken: one fanned kind with inferred,
   `@gpu`-asserted vectorization.
2. **(Resolved) Fanned units have no scalar out-params** — `each`/`map` write columns
   or `reduce`; out-params live only on `proc` (call-once) (§3).
3. **(Resolved) Effects are bounded *descriptors* + a statically-chosen executor, not
   algebraic effects.** `perform`/`resume`/`handler`, dynamic install-stacks, and effect
   hoisting are dropped (§5). funcs build descriptors (the reuse keystone); a `proc`/`each`
   executes at the leaf, or a `system` runs an executor proc to drain a command pool;
   primitives have runtime executors.
4. **(Resolved) `proc` (and `each`) is irreducible — the executor role.** A descriptor is
   inert; something impure must execute it, and that is the call-once `proc` / fanned
   `each`. `func` can't (pure), `system` doesn't compute. So the performing leaf is the
   one role nothing else fills (§5).
5. **Executor binding — by name vs. compile-time callback vs. a small static table.** §4
   says executors bind statically; the exact surface (always a named proc the system runs,
   a `proc`-typed param monomorphized per system, or a tiny compile-time dispatch table
   for descriptor-kind → executor) is open.
6. **Request-response effects as producer stages.** §8.6 models *input* as a column a
   producer stage fills — clean for "give me this frame's input." Does *every*
   value-returning effect fit the producer-stage / fill-a-column shape, or are some
   genuinely mid-computation (a leaf needs a value *now*, derived from prior work in the
   same body)? If the latter, what's the in-leaf form — a direct intrinsic, or a
   descriptor executed inline?
7. **Error/abort as data.** With no unwinding call stack, how does "on failure, run
   recovery" express? Likely: errors become data on the blackboard + a recovery stage the
   system runs. Needs design.
8. **Fixed-timestep / phased scheduling** (§8.5). Does `schedule` get fixed-rate vs.
   per-frame phases, or does accumulator logic live in a one-shot proc?
9. **How much `if` can actually be removed?** Between query-selection and `select`,
   the residual `if` is leaf-internal early-return. Goal: make it the rare exception.
10. **Dynamic-predicate effectful selection** (§6) needs a concrete compaction/partition
    op surfaced to the language, not just the IR.
11. **The blessed-intrinsic set** (§2). Intrinsics are the *closed* set of pure value
    primitives (`select`, arithmetic, slicing) — *not* formatting or `memcpy` (those write
    memory → effects), and *not* foreign (there is no `extern func`). What's left to
    curate: exactly which few primitives are blessed, and the exact set of `os.*`
    syscall primitives (impure-leaf-only).
12. **Legibility of leaf calls.** A leaf's intrinsic/extern/executor call still shares the
    syntax `name(in)(out:)` with a (banned) arche-proc call; only the compiler tells them
    apart. Want a call-site marker so flatness is *visible*, or is compiler-only
    resolution enough?
13. **(Resolved) Formatting is an effect, fed by a pure descriptor.** Byte production
    writes memory, so it can't be a pure buffer-writing func, and there is no `extern
    func`. Instead a pure `func` *builds* a `Fmt`/`Line` descriptor (spec + bounded args —
    §5), and an executor (runtime or a proc the system runs) renders the bytes and writes
    them. Pure code builds the plan; the executor produces bytes.

## 10. Honest costs (so we're not selling magic)

- **DRY dies for arche-proc reuse.** Reuse-by-calling between arche procs
  (`respond_text`→`respond`) becomes inline-or-duplicate — *but* most reuse moves to the
  func world (descriptor-builders compose freely), so the loss is only the effectful tail.
  (The io/net `@allow(proc_could_be_func)` wrapper towers are *not* an example — under §2
  those become a one-hop effect + runtime executor, so they don't exist to lose.)
- **Transient state needs a home.** Stack locals become pool columns or system-scope
  bindings — more upfront data modeling. Array columns (e.g. receive/send buffers)
  reserve their max size per row, always: a 256-connection pool with 1 KB in+out
  buffers costs 512 KB whether the connections are live or not — the no-heap tax,
  paid up front instead of per live request.
- **N=1 ceremony.** An inherently sequential single lifecycle (one CLI run, one
  connection) is fine as one long flat proc; forcing it through a pool-of-one pipeline
  is overkill. Where that cutoff lives — and whether the language or the author picks —
  is unresolved.
- **Heterogeneous dispatch.** "Branch chooses data" is free when arms differ only in
  bytes; when arms differ in *effects*, you need a stage per kind or one fat inlined
  proc.
