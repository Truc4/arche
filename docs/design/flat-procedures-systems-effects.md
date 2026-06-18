# Flat Procedures — Systems, Effects, and Data-Oriented Composition

> **Status: speculative design exploration.** Nothing here is decided or implemented.
> This captures a line of thinking about a substantial change to arche's execution
> model — possibly large enough to be its own language. It records the thesis, the
> resulting language shape, the open forks, and how *real* code in arche /
> arche-web-server / arche-rpg would migrate.

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
| `func`     | pure value — call-once         | nests & recurses freely                         | unbounded (the one opt-in overflow source — §7) |
| `proc`     | effect — call-once `(in)(out)` | funcs + intrinsics; performs effects; never calls a proc/each | depth 1 |
| `each`     | effect — fanned over a pool    | funcs + intrinsics; performs effects; never calls a proc/each | n/a (runtime owns the loop) |
| `map`      | pure — fanned over a pool      | no calls; branch-free (`select`)                | n/a (runtime owns the loop) |
| `system`   | composer                       | calls procs, `run`s each/map, installs handlers | — |
| `schedule` | top-level                      | registers systems                               | — |

**Effect call-depth is exactly 1** (system → leaf, `proc` or `each`). All remaining depth lives in the
pure functional world. The ban makes the *effectful* stack a compile-time constant —
the pragmatic win, developed in full in §7 (a *certified maximum stack size*) — and
forces procedural code flat — the conceptual win.

**The outside world is an effect, and foreign code is *always* an effect.** Touching the
world or calling foreign C is never a free inline call — it is reached by *performing* an
effect, handled at the boundary (where the real syscall / C-call lives). **There is no
`extern func`:** foreign code is always `extern proc`, i.e. effectful. That keeps C from
being the least-friction way to write logic — the friction sits squarely on *doing*, by
design.

So the leaf-callable **floor** is just: native `func`s and a *closed, language-blessed
set* of **intrinsics** — pure value primitives (`select`, arithmetic, slicing).
Everything that *does* something is an effect: I/O, all foreign code, structural pool
changes (`insert`/`delete`, applied at a schedule barrier), and byte production
(formatting writes memory, so it's an effect too — §5). The line is the **contract, not
the implementation**: an op is an effect iff its behavior depends on something beyond its
arguments — the world, shared state, or which handler is installed; a total
single-meaning function of its arguments is pure.

A `func` still participates in effects *without becoming impure*: **a func may *return* a
first-class effect-value** (a description), which a `proc` later *performs* (§5). Pure
code builds the plan; the handler does the work. Systems compute nothing, so they call no
intrinsics; they wire and mark the barriers where deferred effects flush. The C frame's
stack cost rides on its handler (§7).

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
render_row :: each query { id }                    // per-row; writes columns + performs effects
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
(fanned). `each` has **no out-params** — its output is column writes, performed effects,
or a `reduce`; out-params belong to `proc` and effect ops only.

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

A **system is pure glue**: it `handle`s effects, `run`s procs/maps, and sequences
steps. **No `for`, no `if`.** Iteration is *always* `run` — the runtime owns the loop
(arche already has no `for … in`; collection iteration is `run` + map). Counted loops
with no data live *inside* a proc as C-style `for`. Branching for *which data* lives
in queries (see §6), not in the glue.

```arche
report :: system {
  handle Out with to_file("out.txt");   // flat preamble — see §5
  run render_row;                        // fan the effectful leaf over the pool
}
```

`run mapname` → pure kernel, vectorized. Running an `each` → effectful, sequential,
effects resolved by installed handlers. Uniforms are bound at the run site:
`run integrate(dt);`. Write **one `run` per line** — never several stacked on a line; a
row of `run a; run b; run c;` reads as one confusing statement, so each gets its own line.

**Params, and how systems touch them.** Params are very much still in use: a fanned
`map`/`each` takes **uniforms** in `(…)` bound at the run site, and a **call-once**
`proc` keeps its full `(in)(out)`. A `system` *invokes* call-once procs and **threads**
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
built-in semantics of its own. It is normally a `system` (so it can `handle` + `run` +
compose); a bare `main :: proc()()` is allowed for the trivial case — it works only
because the effects it performs (e.g. printing) are the runtime's *ambient* primitives
(§5), and the moment it needs to install a handler or sequence steps it becomes a
`main :: system { … }`.

---

## 5. Algebraic effects (how data/effects pass between procs)

With proc→proc banned, the thing a proc-call used to do — *invoke a named action with
args* — is replaced by **performing an effect**. An effect is **a callback you didn't
have to pass**: the proc names an *operation*, not a concrete callee; a *handler*
installed by the composer decides what it does.

```arche
effect Out { emit :: op(fmt: []char, args: ...)(); }   // a format spec + args — the handler renders

render_row :: each query { id } {                    // performs {Out} — inferred, not written
  emit("row %d\n", id);                              // perform — no concrete sink, no rendered bytes yet
}

to_file :: handler Out(path: []char) {
  setup           { os.open(path, WRITE)(fd:); }     // resource lifecycle owned by the handler
  emit(fmt, args) {                                  // the handler is effectful — it renders, then writes
    buf: [256]char;
    n := /* render fmt+args into buf */ ;
    os.write(fd, buf[0: n]);
    resume;
  }
  teardown        { os.close(fd); }
}
```

Three moving parts: **perform** (jump out to the nearest handler), **handler** (catch
+ act), **resume** (return to just after the perform). Operations respect arche's
out-param convention — an op can have an out-list, which the *handler* writes before
`resume`:

```arche
effect Config { get_limit :: op()(limit: int); }
fixed_limit :: handler Config(n: int) { get_limit()(limit) { limit = n; resume; } }
```

### What a handler is (and isn't)

A handler is a **leaf, not a composer** — it has exactly a `proc`'s powers (call funcs,
use intrinsics incl. the handler-only `os.*`, write state, fail via a policy) plus what its position grants:
**`resume`** (hand control back to the performer) and an optional `setup`/`teardown`
lifecycle bracketing its install extent. It is *not* "a proc that can call procs" — the
proc→proc ban applies to it too: a handler cannot call an arche proc, cannot `run`,
cannot sequence other units. Concretely it is **one leaf body per operation, bound to
an effect** — the reactive dual of a `proc` (the same leaf body bound to a
`run`/schedule site, which *initiates* instead of reacting). You never *call* a
handler; you `perform` an effect and the runtime routes it to the handler the
install-stack has bound.

So "a handler reaching another handler" reduces to "can a handler itself *perform*
effects?" — and here the stance is:

**Probably: handlers are terminal.** *(We'll adopt this unless it turns into a genuine
blocker.)* A handler may use externs + funcs + state + policies, but may **not** perform
further arche effects. That keeps it a true leaf of the effect graph and pays off in
§7: the handler-chain term collapses to a single frame and the acyclicity obligation
vanishes. The expressiveness cost is small — a handler that needs to fail uses a
**policy** (the in-leaf failure mechanism every leaf has); one that needs another
resource calls that resource's **extern** directly. Composing effectful steps across
handlers is then unambiguously the **system's** job (sequence procs, each performing its
own effect) — never a handler reaching into another.

*Alternative, if terminal proves too strict:* permit **outward-only** performs (a
handler installed at level N may perform only effects bound further *out*, never itself,
never inward). More composable, but it reintroduces the §7 handler chain and makes
acyclicity a real check rather than a freebie.

### Design rules that keep it arche-shaped

- **Tracked, not OCaml-5-style.** OCaml 5's defining features — *untracked* effects
  and *first-class one-shot continuations* — are exactly the parts that break arche:
  untracked hides impurity (worse than today's visible `proc`/`func` split), and
  reified continuations need heap/stack capture (violates no-dynamic-allocation).
  Instead: **tracked** (so the compiler keeps funcs pure and gates maps), and
  **tail-resumptive only** (the handler always `resume`s at its tail → compiles to
  "call handler, return, continue" → flat, no captured stack, no heap).
- **Inferred, not annotated.** Because procs can't call procs, a proc's effect set is
  *whatever it textually performs and nothing more* — local, shallow, no fixpoint over
  a call graph. So the compiler **infers** it and the tooling *displays* it. `uses
  {Net, Fail}` stays available as an optional checked contract on public signatures.
- **Checked at the install boundary.** A `system` must handle every effect performed
  in its extent, or it's a compile error ("`Out` performed but not handled").
- **No nesting in proc bodies.** The pyramid-of-`with` problem is avoided by a rule:
  **no construct both performs and installs.** Procs only *perform* (flat body);
  systems install handlers as a **flat preamble** (`handle A with …; handle B with
  …;`) and the runtime composes the stack. Per-step override exists for one-offs:
  `render_summary() with to_buffer(cap);`.
- **funcs stay pure.** A func's inferred effect set must be empty — *except* the
  pure, compile-time, tail-resumptive effect arche already ships: the **policy
  system** (`!clamp`/`!zero`/`!abort`). That's literally a `Fail` effect resolved at
  the op site. Generalizing *that* (tracked, pure handlers, statically resolved) is
  the only effect-like power funcs should get — it keeps them GPU-safe.

### Runtime effects & the OS floor

Not every effect needs a user-written handler. The **primitive effects** — `File`,
`Net`, `Time`, randomness, `Fmt` (byte rendering), and structural `insert`/`delete` —
have **ambient handlers provided by the runtime** (installed at the root), so a leaf may
perform them with no `handle` in scope. More broadly, **much of the stdlib (`fmt`, `io`,
`net`) is effect declarations + handlers**: the pure surface is `func`s that build
effect-values + data types, and the handlers (mostly runtime-provided) do the rendering
and syscalls (§8.2). *User-defined* effects (e.g. an `Out` that writes formatted
lines to one chosen file) still need a `system` to install a handler.

The actual OS boundary is `os.*` (`os.read` / `os.write` / `os.open` / `os.close` /
`os.syscall`): these are **handler-only intrinsics** — the single layer that issues a
real syscall, usable *only inside a handler*, never from a general leaf. A general leaf
*performs* a high-level effect (`File.read`, `Net.send`, `Gfx.draw`); that effect's
handler — runtime-provided or user-written — bottoms out in `os.*`. This is what §2's
"the handler is where the C-call lives" cashes out to, and it is why handlers stay
terminal: they use the `os.*` *intrinsic*, they do not perform further arche effects.

### Structural effects (`insert` / `delete`)

`insert`, `delete`, and live-count changes are the effects that change a pool's *shape*
— which rows exist — rather than a row's *values*. They're handled specially because of
an ECS hazard: **you cannot structurally mutate a pool you're currently iterating** (a
`run`/`map` over it) — that invalidates the iteration, and races if stages run in
parallel on the cores backend. So:

- **Performed in a leaf, applied at a barrier.** A leaf *performs* the structural change
  (per-row logic decides what to spawn/remove); it is **queued**, and the runtime
  **flushes it at the schedule/system seam** (between `run`s). New rows become visible to
  the *next* stage. This is exactly Bevy's deferred "Commands."
- **Immediate vs. deferred is by context, and statically decided.** *Immediate* when the
  leaf is **not** fanning the target pool (e.g. the router building its trie in a `once`
  system — works as today, and `insert` returns the slot synchronously). *Deferred* when
  performed **inside a `run` over that pool**. The compiler knows which case applies.
- **The handler is the runtime.** No user handler is written — the runtime knows how to
  add/remove a pool row. These are effects with a *built-in* handler, like the core I/O
  effects of §8.2.
- **Overflow is a policy.** A static pool can fill; `insert` then follows the pool's
  `overflow_policy` (which arche already has) — the same in-leaf failure mechanism as
  everything else. The command queue is itself a static, per-tick-bounded buffer (no
  heap) with its own overflow policy.

**The synchronous-slot tension.** A *deferred* `insert` cannot hand back a usable row
index — the row does not exist until the flush. The router's "insert → get slot → link
it in immediately" pattern relies on the *immediate* case (it runs in setup, not fanning
the pool). If you ever need spawn-and-immediately-reference *mid-fan-out*, you need
reserved/promised ids (Bevy does this) or you simply don't get the index until after the
barrier. §8.4's `insert Conn rows` / `delete done rows` depend on these deferred-at-barrier
semantics: spawns land for the next stage, deletes don't disturb the current fan-out.

### Effects as values (a func may return one)

An effect need not be performed where it's named. **Effects are first-class values:** you
can construct an *unperformed* effect — a description — and return it. This stays pure,
because **returning an effect ≠ performing it**:

```arche
fmt_int :: func(x: int) -> Emit { return Emit("%d", x); }   // pure: builds a description, performs nothing

show :: each query { id } {
  perform fmt_int(id);     // a proc/each performs the effect the func returned
}
```

Pure code (including `func`s and `map`s) builds the *plan*; a `proc`/`each` (or the
runtime) *performs* it; the *handler* does the actual work — e.g. rendering bytes and
writing them. This is the clean answer to "how does pure code do effectful-looking
work": it **returns** the effect, it doesn't perform it. (It's the IO-value / free-monad
shape — see below.)

Two constraints keep it arche-shaped:
- **Bounded.** An effect-value is a fixed-size description (op tag + bounded args) — no
  heap. A format effect carries a spec + a small arg list, *not* rendered bytes.
- **First-order.** A func returns *an* effect; a proc performs it. Effect-values don't
  chain monadically (`bind`) — that would drag in the continuation/heap machinery the
  design avoids.

And it's why **byte production is never a pure buffer-write** (writing memory is an
effect): a pure formatter *returns* a description, and the **handler** is the only place
bytes are rendered and written.

### Relationship to monads

Effects and monads solve the same problem (structured effects in a pure setting).
Monads bake the effect into the return type (`IO a`, `State s a`) and need
transformer-stacking + closures + HKT to combine — a poor fit for a no-heap language.
arche already *hand-rolls* the specific monads it needs without naming them:
`Result`/`Either` = out-param `ok` flags + policies; `State` = the blackboard / pool
columns; `IO` = a `proc` + the `schedule` (a proc is an `IO ()`; the schedule is the
interpreter — i.e. the Free-monad shape). Statically-resolved algebraic effects are
the thin, heap-free abstraction over what arche is already doing.

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
so:

```
max_stack  =  spine(schedule + system frames)
           +  max(proc frame size  over all procs)        // each statically sized — no heap
           +  Σ(handler frames along the longest effect chain)
```

Every term is a compile-time constant. (Under the *terminal-handler* stance in §5, the
Σ-chain term is just **one** handler frame and condition 2 below is automatically met —
the chain + acyclicity machinery only applies under the permissive alternative.)

**How — the conditions** (each closes an otherwise-unbounded source of depth):

1. **No proc→proc / no proc recursion** — the core ban. Removes effect-spine recursion.
2. **Acyclic effect→handler graph** — the effect-world twin of the ban. A handler may
   only perform effects resolved *outside* it; a cycle (handler-for-`A` performs `B`,
   handler-for-`B` performs `A`) would rebuild unbounded mutual recursion. Acyclic →
   each handler appears at most once on a chain → depth ≤ installed-handler count.
   Statically checkable.
3. **Statically resolved effects/calls** — no dynamic handler lookup, no runtime
   proc-values. This is the *same* decision that gives the flat stack (§5); dynamic
   dispatch would reintroduce an unknown graph. One choice, both payoffs.
4. **Funcs carry no unbounded recursion** — funcs are the one place the design opts
   *back into* depth (the pure compositional world). A non-recursive func is fine (its
   call graph is also a DAG, still summed). A *recursive* func must carry a static depth
   bound (`@depth(≤N)`) to stay in budget, or it is excluded from the guarantee. So the
   real rule is **"no unbounded recursion anywhere,"** not "no funcs."

**How — bound vs. fit (two steps).** Conditions 1–4 make `max_stack` *finite and
computable* — but finite ≠ fits: a long chain or fat frames (big `[K]char` locals)
could still exceed the physical stack. So the guarantee is two steps:

1. compute `max_stack` (possible *because* the graph is acyclic + statically resolved);
2. **verify `max_stack` ≤ the declared budget, and reject the program otherwise.**

Step 2 is what turns "computable" into "impossible" — by refusing to emit a binary that
wouldn't fit. The knob:

- `arche build --max-stack=8K` — a whole-program ceiling; compile error if exceeded.
- `@stack(≤512)` on a proc or system — a checked, *composable* contract (a scheduler
  sums its leaves' declared bounds), verified like a type.

**Edges.**

- **FFI cost rides on its handler.** Foreign code is reached through a handler (§2),
  so the C frame is part of *that handler's* budget, declared via its `@stack`. An
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

**After:** the transform is *already columnar* → a `map`. `load`/`write` become flat
leaves that *perform effects* (and use intrinsics/funcs), never arche procs. The per-row
write loop becomes a fanned `each`, and `main` dissolves into the `#schedule`.

```arche
bucket :: map query { price, price_bucket } { price_bucket = price / 10.0; }

render_row :: each query { price, quantity, price_bucket } {        // fanned; performs {Out}
  emit("%f,%d,%f\n", price, quantity, price_bucket);               // perform Out — spec+args, handler renders
}

load_tx :: proc()() {              // one-shot ingest: performs File.read (ambient runtime handler),
  /* read + parse via funcs, fill Transaction columns — no arche-proc calls */
}

to_csv :: handler Out(path: []char) {              // user effect → user handler; bottoms out in os.*
  setup           { os.open(path, WRITE)(fd:); }   // os.* = handler-only intrinsic (the OS floor)
  emit(fmt, args) {                                // effectful handler: render, then write
    buf: [256]char;
    n := /* render fmt+args into buf */ ;
    os.write(fd, buf[0: n]);
    resume;
  }
  teardown        { os.close(fd); }
}

write_out :: system {
  handle Out with to_csv("…/arche_output.csv");    // install the user handler for this extent
  run render_row;                                    // fan over Transaction; each emit → to_csv → os.write
}

#schedule {
  once load_tx;     // read + parse → fills Transaction (File handler is ambient)
  once bucket;      // compute (map: vectorizes / cores)
  once write_out;   // serialize + write (fanned each + user handler)
}
```

### 8.2 The IO wrapper towers — dissolve into effects

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

**After:** I/O is an *effect*, not a proc-call tower — §2 says foreign code is reached
by *performing* an effect, never called inline. So the tower doesn't "collapse," it
**dissolves**: there is no `read`→`fread`→`syscall` chain, because reading *is* a
performed effect and the syscall lives in a runtime-provided handler at the boundary:

```arche
effect File { read :: op(f: fd, buf: []char)(out: []char); }   // declared once

load_tx :: proc()() { read(fd, buf)(chunk:); /* … */ }         // PERFORM — no syscall, no wrapper

sys_files :: handler File {                                     // runtime ships this; the syscall lives HERE
  read(f, buf)(out) {
    os.syscall(0, f, buf, buf.length, 0,0,0)(rr:);              // os.syscall = runtime intrinsic, handler-only
    out = buf[0: clamp_lo(i32(rr))];  resume;
  }
}
```

So the `@allow(proc_could_be_func)` wrappers that flagged the smell don't exist in this
model — there is nothing to wrap. The public surface is *one effect declaration*, and
the runtime owns the single handler. (The earlier "DRY dies in the wrapper tier" worry
was an artifact of treating FFI as a free inline call; once I/O is an effect, the
wrapper towers are gone, not duplicated.)

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

`add` calls `new_node` → inline it (it's a wrapper). `resolve` returns a `handler_id`
(data, not a call) and stays as-is. The router migrates with near-zero structural
change — it was already data-oriented.

### 8.4 HTTP request lifecycle — dispatch-as-data + the staged pipeline

`stdlib/http/http.arche`: the dispatch branches to a *different proc per arm* —
proc→proc chosen by data, the hardest case:

```arche
match h {
  handler.home      : respond(conn, hdr, 200, "text/html", home_html, true);
  handler.user      : respond(conn, hdr, 200, "text/html", user_html, true);
  handler.not_found : respond_text(conn, hdr, 404, "Not Found\n");
}
respond_text :: proc(conn, hdr, status, msg) { respond(conn, hdr, status, "text/plain…", msg, true); } // proc→proc
```

**Resolution: make the branch choose *data*, not a procedure.** There is no `respond`
proc to call (that would be proc→proc). The branch writes *data* to the row's columns,
and a single later stage sends it uniformly — arche already cut this seam: `resolve`
returns an id, it never calls a handler:

```arche
// inside serve_step — the branch picks DATA, never a proc:
status := 200;  body := home_html;
body   = select(handler_id == handler.user, user_html, body);
// handler.not_found → status = 404, body = "Not Found\n", …
// these land in the row's status/outbuf columns; the actual send is send_step (perform Net.send)
```

**The payoff — connections as a pool, the lifecycle as a staged pipeline.** Once
dispatch is data, the deep per-request *call stack* becomes a wide per-connection
*data pipeline* (how high-throughput servers are actually built). Every transient that
lived on the stack becomes a `Conn` column:

```arche
Conn :: arche {
  fd :: int, stage :: int, nread :: int, handler_id :: int, status :: int,
  inbuf :: [1024]char, outbuf :: [1024]char,        // array columns (cf. TrieNode.seg, Message.text)
}
[256]Conn(0);                                        // 256 live conns, static, no heap

accept_new :: proc()()                          { /* perform Net.accept; insert Conn rows (structural — flush at barrier) */ }
recv_step  :: each query { fd, inbuf, nread }   { /* perform Net.recv → inbuf */ }
parse_step :: each query { inbuf /*, method/target cols */ }  { /* fill columns (pure intrinsics/funcs) */ }
route_step :: each query { /* target →  */ handler_id }       { /* resolve → handler_id (data) */ }
serve_step :: each query { handler_id, status, outbuf }       { /* branch picks DATA → fill status/outbuf */ }
send_step  :: each query { fd, outbuf, status }               { /* perform Net.send; perform delete on done rows */ }

serve :: system {                  // Net is an ambient runtime effect — no explicit handle needed
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
The per-ball draw becomes a fanned `each` (`run draw_ball`) over the `Player` pool — the
`gfx` handle is an installed effect, not a threaded arg. Seeding and window setup are
`once` systems. The fixed-timestep accumulator is the one genuinely tricky bit (it's
control logic about *when* to run sim vs. draw) — it likely needs schedule-level
support for fixed-rate vs. per-frame phases, or stays inside a small one-shot proc.

```arche
// Gfx is a user effect: { clear(color); draw(x, y, color); present() }
draw_ball    :: each query { pos, color } { draw(pos.x, pos.y, color); }  // perform Gfx.draw
step_clear   :: proc()()                  { clear(0x101010); }            // one-shot; perform Gfx.clear
step_present :: proc()()                  { present(); }                  // one-shot; perform Gfx.present

frame :: system {
  handle Gfx with window();      // user handler opens the window in setup; issues gfx/os.* at the boundary
  run game.step;                 // sim (a map)
  step_clear();                  // one-shot
  run draw_ball;                 // fan over Player; each performs Gfx.draw
  step_present();                // one-shot
}
#schedule {
  once seed_world;
  loop frame;                    // fixed-timestep policy: open Q (§9)
}
```

### 8.6 Reuse: one set of leaves → three programs

The payoff of migrating to leaves + effects: one tiny set of building blocks becomes a
game, a renderer, and a test. `play`/`export` differ only in *handlers*; `verify`
differs only in *composition*. The logic never changes.

**Shared building blocks** — written once, knowing nothing about keyboards, windows, or
files (names commit to *intent*, never mechanism — §5):

```arche
effect Input { read :: op()(dir: vec2); }              // "give me a movement command"
effect Frame { draw :: op(x: int, y: int, c: int)(); } // "place a sprite"

control   :: each query { vel }        { read()(d:);  vel = d * SPEED; }   // perform Input.read
integrate :: map  query { pos, vel }   { pos = pos + vel; }                // pure physics
render    :: each query { pos, color } { draw(pos.x, pos.y, color); }      // perform Frame.draw
```

**Handlers** — the mechanisms, each bottoming out in `os.*` (handler-only intrinsics):

```arche
keys :: handler Input { read()(dir) { os.poll_keys()(dir:); resume; } }              // live keyboard
tape :: handler Input(path: []char) {                                                // recorded commands
  setup { os.open(path, READ)(fd:); }   read()(dir) { os.read(fd, dir)(dir:); resume; }
}
screen :: handler Frame { setup { os.win_open()(w:); } draw(x,y,c) { os.blit(w,x,y,c); resume; } }
png    :: handler Frame(dir: []char) {                                               // capture to disk
  draw(x,y,c) { fb_put(x,y,c); resume; }   teardown { os.write_png(dir, fb); }        // fb = handler state
}
```

**Two systems, two programs — identical leaves, swapped handlers:**

```arche
play :: system {                          // interactive game on screen
  handle Input with keys();
  handle Frame with screen();
  run control;
  run integrate;
  run render;
}

export :: system {                        // deterministically re-render a recording into image files
  handle Input with tape("session.in");   //  input from a recording, not a keyboard
  handle Frame with png("frames/");        //  frames to disk, not a window
  run control;
  run integrate;
  run render;
}
```

`control`/`integrate`/`render` are byte-for-byte the same in both. `play` is a
keyboard-driven game; `export` turns a recorded session into PNGs — deterministic,
headless. The leaves never learned what a keyboard or window is; the systems chose, by
which handlers they installed.

**A third program — same pieces, *recomposed*:**

```arche
verify :: system {                        // deterministic physics test — no rendering at all
  handle Input with tape("session.in");
  run control;
  run integrate;            //  `render` simply isn't composed in
  check_invariants();                      //  one-shot test leaf asserts the final positions
}
```

`verify` reuses `control` + `integrate`, drops `render`, adds an assertion. One set of
leaves → **game, renderer, test suite**, with zero changes to the logic. The line a
programmer feels first: **you test the real game logic with no mocking framework — you
install a `tape` handler instead of `keys`.**

---

## 9. Open questions

1. **(Resolved) The effectful fan-out is `each`** — its own kind, distinct from the
   call-once `proc` (§3). Alternative not taken: one fanned kind with inferred,
   `@gpu`-asserted vectorization.
2. **(Resolved) Fanned units have no scalar out-params** — `each`/`map` write columns
   or `reduce`; out-params live only on `proc` (call-once) and effect ops (§3).
3. **Handler precedence + lifecycle ordering.** When a system installs several
   handlers in one preamble: install/`setup` order, `teardown` order, and what happens
   to `teardown` when a step aborts (`!abort`).
4. **Error/abort as data.** With no unwinding call stack, how does "on failure, run
   recovery" express? Likely: errors become data on the blackboard + a scheduled
   handler. Needs design.
5. **Fixed-timestep / phased scheduling** (§8.5). Does `schedule` get fixed-rate vs.
   per-frame phases, or does accumulator logic live in a one-shot proc?
6. **How much `if` can actually be removed?** Between query-selection and `select`,
   the residual `if` is leaf-internal early-return. Goal: make it the rare exception.
7. **Dynamic-predicate effectful selection** (§6) needs a concrete compaction/partition
   op surfaced to the language, not just the IR.
8. **The blessed-intrinsic set** (§2). Intrinsics are the *closed* set of pure value
   primitives (`select`, arithmetic, slicing) — *not* formatting or `memcpy` (those write
   memory → effects), and *not* foreign (there is no `extern func`). What's left to
   curate: exactly which few primitives are blessed, and whether `os.*` is the
   handler-only syscall primitive. (Stdlib = effect-declarations + handlers is settled —
   §5.)
9. **Legibility of intrinsic-calls.** An intrinsic call still shares the syntax
   `name(in)(out:)` with a (banned) arche-proc call; only the compiler tells them
   apart. Want a call-site marker so flatness is *visible*, or is compiler-only
   resolution enough?
10. **(Resolved) Formatting is an effect, fed by pure descriptions.** Byte production
   writes memory, so it can't be a pure buffer-writing func, and there is no `extern
   func`. Instead a pure `func` *returns* a `Fmt`/`Emit` description (spec + bounded
   args — §5), and the **handler** renders the bytes and writes them. Pure code builds
   the plan; handlers produce bytes.

## 10. Honest costs (so we're not selling magic)

- **DRY dies for arche-proc reuse.** Reuse-by-calling between arche procs
  (`respond_text`→`respond`) becomes inline-or-duplicate. (The io/net
  `@allow(proc_could_be_func)` wrapper towers are *not* an example — under §2 those
  become a single effect + runtime handler, so they don't exist to lose.)
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
