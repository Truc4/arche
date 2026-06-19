# The Flat Effect Model

### effects as values, procedures as leaves — side effects in arche without monads or algebraic-effect machinery

> **Status:** design exploration. The schedule section (§9) is genuinely open — it's written as
> questions with proposed answers, not settled law.

## Abstract

Most languages model side effects with heavy machinery: monad transformers, or algebraic effects
with a runtime handler stack and first-class continuations. arche needs neither. It models effects
with **three kinds** (`func`, `proc`, `system`) and **one primitive**: an effect is a *bounded value*
that a pure function builds and an impure leaf runs. Flatness isn't even a rule you enforce — a proc
simply has no way to call a proc, so the effect call graph is depth-1 by construction.

The surprise is that this isn't a sacrifice. The model is **flat** (the effect call graph is depth-1
by construction), **explicit** (no late-bound jump to chase), **heap-free** (effects are fixed-size
descriptors), **data-oriented** (effects compose into columns and drain in a fanned kernel), and
**statically dispatched** (a compile-time vtable, not a runtime search). And every one of those
properties falls *out of* arche's existing constraints — no heap, static pools, ECS columns — rather
than fighting them.

This document explains what the model is, why each piece exists, and why it's specifically good in
arche. It ends with the one part we haven't nailed: the schedule.

---

## 1. The problem: effects want to nest, and nesting is the enemy

Procedural code rots in a predictable way. You write `handle_request`, which calls `parse`, which
calls `read_field`, which calls `decode`, which calls `os_read`. Five frames deep, half of them doing
I/O, and now your stack depth, your effect set, and your control flow are all a function of a call
graph nobody can see at a glance.

arche's thesis is simple: **nesting and recursion belong to the *functional*
world; procedural code should be a flat sequence of steps.** Pure functions are where depth is fine —
it's just math, and the cost is paid in a sandbox. Effects are where depth hurts — every level is a
real frame doing real I/O at runtime.

So the load-bearing move is a single structural fact:

> **A `proc` cannot call a `proc`** — not because a rule forbids it, but because the language gives it
> no way to. Effects are inert values; only a `system` invokes a proc.

This sounds draconian. It is the whole game. It makes "procedural code is a flat list of steps" a
*structural fact* instead of a style guide you can `@allow` away — there's no syntax to violate, so
nothing to check. The runtime effect spine is depth-1 by construction — which makes the whole-program
stack bound *trivial to state* (it's just `max(system) + max(proc) + max(extern)`). To be clear, depth-1
isn't what makes the bound *provable* — static dispatch and monomorphization are (§6); a non-recursive
deeper spine would still be computable. Flatness buys legibility and a one-line bound, not the bound's
existence.

The obvious objection — *"then how do two procs share an effectful sub-step?"* — is the rest of this
document.

## 2. Why not algebraic effects (Koka, Eff, OCaml 5)

If you've read the PL literature, algebraic effects are the answer you'd reach for — they're the
research community's current favorite, implemented in languages like **Koka** and **Eff** and, since
2022, in **OCaml 5**. They're a bad fit here. Their defining feature is
**dynamic, non-local dispatch**: you `perform` an effect, and *which* handler catches it is decided
at runtime by an install-stack that may sit arbitrarily far up the call chain, often with multi-shot
continuations. That machinery is exactly the reference-chasing arche is trying to delete, and the
continuations want a heap arche doesn't have.

arche keeps the *vocabulary* (effect, perform, handler) only loosely and throws away the engine. Its
dispatch is **static and local**: the implementation of an effect is chosen at compile time, by the
sibling that scheduled the work. That's closer to a **compile-time vtable / dependency injection**
than to Koka or Eff. We'll come back to this — it's what makes effects testable (§6) without making
them mysterious.

## 3. The model: three kinds, one keystone

| kind | what it is | may call | may **not** | reached by |
|---|---|---|---|---|
| `func` | pure value-world | funcs; **wraps an extern into a value** | run an effect; wrap a proc | called freely (nests, recurses) |
| `proc` | an effect — a flat leaf | funcs; terminal externs (one hop) | **call a proc** | only a `system` runs it |
| `system` | the composer | procs, funcs, full control flow | **call a system** | only `#schedule` |

And the keystone — the thing that makes the whole shape work:

> **A pure `func` can build an effect as a *value* and hand it out.** Building it runs nothing.

```arche
// pure: builds a description of "write this", returns it; performs no write
write_b :: func(fd: int, buf: []char) -> Eff(int, int) { return wrap fwrite(fd, buf); }
```

`Eff(int, int)` is the type of a **wrapped, not-yet-run effect**: a bounded value standing for "an
effect that, when run, yields these results" — here the two out-slots `(n, err)` of `fwrite`. Think
Haskell's `IO a`, adapted to arche's multi-out-slot convention. The value is inert. You **run** it by
calling it with out-slots — there's no special keyword; running an effect *is* providing its out-params:

```arche
eff := write_b(fd, line);   // PURE: build the effect value, bind it like any other value
eff(n:, err:);              // IMPURE: run it — the out-slots come from running, not from the func
```

That two-line split is the entire model in miniature:

- **`func` + `wrap`** is the *value* world. Effects-as-data live here. They nest, compose, get passed
  around, get chosen conditionally, get stored — all pure.
- **`eff(out:)`** is the single impure act: calling an `Eff` with out-slots turns the inert value into
  a real call and binds its results. Out-params belong to *running* an effect, never to a func — which
  is also why a pure func can never have effect out-params, only an `Eff` return.

Because reuse lives entirely in the `func` world, **a proc never needs to call a proc — and the
language gives it no way to.** You can't factor a shared effectful step into a sub-proc; you factor it
into a func that builds the `Eff`. So flatness isn't a checked rule with an error message — it's
**unspellable**: an effect is inert data (it can't run anything), and a proc has no syntax to invoke
another proc. The one back door — wrap a proc in a func, then run that from another proc — is closed by
construction: **`wrap` ingests only an `extern`** (a foreign leaf with no arche body), never a proc.
The reason is ontological, not a rule bolted on: an extern is *inert without its out-slots* (just an
opcode plus inputs — a value), whereas a proc minus its out-slots is a *suspended computation*, exactly
the runnable thunk the value world refuses to contain. So there is no expression anywhere that turns a
proc into an `Eff`. The proc shrinks to a thin imperative shell: *run these effects, react, report via
out-params.*

```arche
// procs = flat effect LEAVES: run what funcs built (call it with out-slots), react, expose out-params.
log_header :: proc(fd: int)(ok: bool) {
  line(fd, "id,price,qty")(n:, err:);   // line() is a func building one Eff; the trailing (out:) runs it
  ok = (err == 0);
}
```

And the `system` is the **composer**: it runs leaves and threads their out-slots between them. It
performs no effect itself — the doing is in procs.

```arche
serve :: system {
  open_conn(8080)(fd:, ok:);          // run a leaf, capture its out-slots
  if ok { handle_msg(fd)(done:); }    // thread fd into the next leaf
}
```

Three kinds, and they don't overlap once you stop putting effects in the system: **func builds, proc
does, system composes.** A proc is a leaf because nothing inside it can invoke a proc; the one thing
that *can* invoke procs — and thread data between them — is the `system`. That asymmetry (leaf vs.
composer) is the whole shape, and it falls out of the syntax, not a rule.

(Whether `proc` even needs to be a written keyword is a smaller, open question: a body that runs an
`eff` is impure and could be *inferred* as a proc. The trade is arche's explicit-coloring ethos — an
inferred color is a hidden color — so this doc keeps the marker. Either way, the no-nesting property
is structural, not a checked ban.)

## 4. The primitive boundary: where effects come from

A careful reader asks: if a `func` only ever *builds* effects and a `proc` only *runs* them, **who makes
the atom?** Where does the very first `Eff` come from? The answer is a hard rule with a soft surface:

> **No arche code can create an atomic effect. Pure code can transform, compose, and choose effects —
> never manufacture one. Every atom is a *primitive*: an irreducible interaction the language cannot
> express in itself.** `wrap`/saturation builds an `Eff` *from* a primitive; it doesn't invent the
> primitive.

This isn't a limitation, it's the definition of an effect. An effect *is* an interaction with something
outside the pure semantics; there is nothing for pure code to "do" that would be an effect. The same is
true everywhere: Haskell has no pure function that conjures a new `IO` — every `IO` atom is an RTS
primitive or FFI (`putStr`, `readMutVar#`, `foreign import`); Rust's I/O all bottoms out in `unsafe` +
a syscall. The payoff is that the effect set is **closed and auditable**: list a program's primitive
declarations and you have listed *every* atomic effect it can possibly perform.

**`extern` is one category of primitive, not the only one.** arche already has a *native* effect
primitive that touches no foreign code: structural mutation — `insert` / `delete` on pools, routed
through the deferred command buffer and flushed at a stage barrier (§6 leans on exactly this). So the
honest statement is "effects come from primitives," and the primitive set today is roughly:

- **foreign calls** — `extern proc(...)(...)` (the C-ABI boundary), and
- **structural mutation** — `insert` / `delete` (pure-arche-native, the command buffer).

### "What happens when everything becomes arche-native?"

Nothing alarming — because *native* splits into two things, and only one ever involved effects:

- **Native *logic*** (parsing, math, sorting, data structures) **was never an effect.** As arche
  reimplements what libc did — `strtol`, `qsort`, JSON, an HTTP parser — those become **funcs**. Going
  native here just means *more pure code*; the effect surface is untouched, and in fact more of the
  program moves into the testable, composable world.
- **Native *I/O*** (a TCP stack, a filesystem, an allocator written in arche) still bottoms out at
  hardware: a NIC register store, an MMIO write, a syscall trap, an interrupt. Those irreducible
  hardware touches are *still* primitives. You write the protocol *logic* as funcs; the "poke this
  device register" stays a leaf.

The consequence is the opposite of effects disappearing — going native **shrinks and sharpens** the
boundary. Today `extern fwrite` hides a pile of libc buffering behind one opaque leaf; native, you
`extern write` (the *syscall*) and all the buffering/formatting libc did becomes arche funcs. The
audited effect list gets *smaller and more honest* — the genuine syscall/MMIO boundary instead of
"whatever libc happened to do."

### Generalizing `extern` → `prim`

`extern proc` currently *means* "foreign C function," but the *concept* is "an irreducible
world-boundary leaf." A freestanding/self-hosted arche wants sibling surfaces for the same concept,
which the effect model wraps identically:

```arche
#foreign {
  write   :: extern proc(fd: int, buf: []char, n: int)(ret: int, err: int)   // C-ABI call (today)
}
#primitive {
  syscall :: prim proc(nr: int, a: int, b: int, c: int)(ret: int)            // trap to kernel
  mmio_w  :: prim proc(addr: int, val: int)()                                // volatile device store
  rdtsc   :: prim proc()(cycles: int)                                        // asm intrinsic
}
```

Each is the same thing the model already handles: an inert, world-touching leaf you compose into `Eff`
and run in a proc. The *model is unchanged*; only the primitive vocabulary grows. And the invariant
holds at every stage of self-hosting: pure code can never mint an atom, so the closed primitive set
always *is* the program's complete, auditable effect surface.

## 5. The applicative/imperative line (the one subtlety worth memorizing)

There are two kinds of effect composition, and arche puts them in two different places.

**Static composition → funcs.** When the *shape* of the effect graph is known up front — "this write,
then a newline" — a func can build the whole thing as one `Eff`. No runtime value flows between the
pieces. This is the **applicative** rung: structure known before execution, which is exactly why it's
safe to inspect, reorder, batch, and run in parallel.

**Result-dependent sequencing → the proc.** When the *next* effect's shape depends on the *previous*
one's runtime result — "read a length header, then read *that many* bytes" — you cannot pre-build it.
You must run the first, look at what came back, then run the second. That's the **monadic** rung, and
it lives in the proc, where running an effect (supplying its out-slots) executes inline and binds the
result:

```arche
handle_msg :: proc(fd: int)(ok: bool) {
  hdr: [1]char;
  read(fd, hdr, 1)(got:, err:);             // run (call with out-slots); non-deterministic — peer decides
  if err != 0 || got == 0 { ok = false; return; }   // react
  len := int(hdr[0]);                         // the peer chose this at runtime
  body: [256]char;
  read(fd, body, len)(got:, err:);            // dependent: shape decided by the first read
  ok = (err == 0 && got == len);
}
```

The slogan: **funcs compose the known part of the effect graph as a value; the proc is the imperative
shell that runs it, branches on what actually came back, and reports out.** That's the func/proc split
done honestly — value vs. computation, à la call-by-push-value, with the monadic tail confined to one
flat leaf instead of smeared across a call stack.

### What collapses into an `Eff` — and what doesn't

A natural question: *can a func collapse a series of steps into a single `Eff`?* The answer is the
applicative/imperative line restated as a rule, and it has three cases:

| a series of… | composed by | the result is | which ordering |
|---|---|---|---|
| effects (externs) | a **func** | one `Eff` value | **static only** (applicative) |
| effects, result-dependent | a **proc** body | nothing — imperative steps | dynamic, but not a value, not reusable |
| **procs** | a **system** | a schedule step | dynamic, branch on out-slots |

- **Yes — a static series of effects collapses into one `Eff`.** This is the intended way two procs
  *share* an effectful step: factor the sequence into a func that composes the wrapped externs into one
  value. When run, it performs them in order and yields the union of their out-slots. Crucially this
  adds **breadth, not depth** — the run unfolds to `system → proc → {extern, extern, …}`, still
  depth-1, because the composition happened in the value world (where nesting is free) and flattens to
  terminal externs with no proc in between.

- **No — a result-dependent series cannot be an `Eff`.** The moment a later effect's *input* is an
  earlier effect's *output* (`read` a length, then `read` that many bytes), you'd need a free *monad*
  carrying runtime continuations — allocation, unbounded depth, exactly what arche refuses. So it stays
  imperative, inside one proc (the `handle_msg` example above).

- **No — a series of *procs* never collapses into a value at all.** A proc isn't a value (a proc minus
  its out-slots is a suspended computation, not inert data), so nothing ingests several procs and emits
  one `Eff`. What *sequences* procs — threading out-slots, branching on results — is a **`system`**, and
  it produces a schedule step, not a value. There is deliberately no path from "several procs" to "one
  value": that path is precisely what would make procs first-class and collapse the flatness.

## 6. Why this is *specifically* cool in arche

Other languages could adopt "effects as values." What makes it sing here is that every piece lands on
something arche already has:

1. **No heap, so `Eff` is a bounded descriptor.** arche has no dynamic allocation, ever. An effect
   value is therefore a fixed-size thing — an op tag plus bounded args plus room for its result slots.
   That's not a limitation bolted on; it's the same discipline as every other arche value.

2. **Columns, so effects compose into a command buffer.** This is the payoff. Because an `Eff` is a
   value, a pure `func`/`map` can build a *column* of them, and one fanned `each` can drain it:

   ```arche
   ops: [N]Eff(int, int);
   ops[i] = write_b(fd, fmt_row(...));   // pure: build effect values into a column (no I/O yet)

   drain :: each query { op } { op(n:, err:); }   // impure: run each (call with out-slots), flat, one kernel
   ```

   arche *already* does exactly this for structural `insert`/`delete` — the deferred "Commands"
   buffer, flushed at a stage barrier. The flat effect model is just that pattern **generalized from
   structural ops to all effects**. You didn't invent a mechanism; you noticed you already had one.

3. **Static dispatch, so the stack bound is a theorem.** No dynamic handler search, no captured
   continuations. The effect spine is `system → proc → terminal extern`, depth 1. The whole-program
   worst-case stack is computable (every callback is monomorphized; there are no runtime fn-pointers),
   so "this program will never blow its stack" is a thing the compiler can *say*.

4. **Static dispatch, so effects are testable without mocks.** The `system` chooses *which* leaf runs
   — statically. So the same reactive logic runs against live input in production and against a
   recorded tape in a test, by swapping one `run` at the composer. No mock framework, no DI container,
   no runtime indirection: a compile-time substitution, which is the only kind arche has anyway.

   ```arche
   play   :: system { run game_step(keys);  }   // live
   verify :: system { run game_step(tape);  }   // recorded — deterministic, headless, real logic
   ```

5. **Data-oriented by construction.** The pure compute (`map`), the effect-builders (`func`), and the
   thin effect leaves (`each`/`proc`) all speak the same column language. Effects don't sit *outside*
   the DOD model in some monadic side-channel; they *are* rows and kernels.

## 7. Worked example: an ETL pipeline, top to bottom

```arche
bucket  :: map query { price, price_bucket } { price_bucket = price / 10.0; }   // pure column op
fmt_row :: func(p: float, q: int, b: float) -> []char {                          // pure row builder
  return f32(p) <> "," <> int(q) <> "," <> f32(b) <> "\n";
}
write_b :: func(fd: int, buf: []char) -> Eff(int, int) { return wrap fwrite(fd, buf); }  // wrap

write_row :: each query { price, quantity, price_bucket, ok } (fd: int) {   // effect leaf, fanned
  eff := write_b(fd, fmt_row(price, quantity, price_bucket));
  eff(n:, err:);
  ok = (err == 0);                                                          // per-row failure as data
}

write_out :: system {                       // composer: open, fan, close; threads fd
  open_csv("…/out.csv")(fd:, ok:);
  if ok { run write_row(fd); close_csv(fd); }
}

#schedule { once load; once bucket; once write_out; }   // the one timeline
```

The shape to notice: *all* the logic is pure (`bucket`, `fmt_row`), the effect intent is a value
(`write_b` → `Eff`), the leaf is a thin proc that runs the effect and records failure as a column
(`ok`), and the system just sequences. There is no `main`, no nesting, no proc calling a proc. Failure
is **data**, not a thrown exception clawing up a stack that doesn't exist.

## 8. Honest costs

No free lunches; here are the bills.

- **The reuse hole is narrow — but real.** A *statically-ordered* effectful sub-step shared by two
  procs **does** factor: collapse the wrapped externs into one func-built `Eff` and run that in each
  proc (§5). What can't be shared is a **result-dependent** sequence — where a later effect's input is
  an earlier one's output. It can't be a func (impure), can't be an `Eff` (that's a free monad —
  unbounded, allocating), and can't be a sub-proc (procs aren't values). So that specific shape is
  either duplicated across procs or lifted to a `system`. Not "DRY shrinks for all effects" — only for
  the monadic tail.
- **Result-dependent sequencing can't be a value.** You can't pre-build `eff2` from `eff1`'s runtime
  result; that case runs the effects inline in a proc and branches (§5). The model is honest that this
  is the applicative→monadic boundary, not a bug.
- **Varargs/formatting is the hard descriptor.** A `printf`-style effect with heterogeneous args
  strains "bounded value." The fix is a bounded *builder* (associative bounded-append, identity =
  empty) rather than a format-string descriptor — a pure algebra, still no heap.
- **"No heap" really means "everything max-reserved."** A command-buffer column is a static `[N]`. You
  pay the ceiling up front. That's arche's deal everywhere, but it's worth saying out loud.

## 9. The open problem: the schedule

Here is the part we have *not* settled. `#schedule` is the single, deterministic timeline — the spine
that says what runs, in what order, once or in a loop. The questions are about how rigid that spine
is and who's allowed to touch it.

**Q: One schedule, or many? Can a module/device own its own schedule?**
Proposed answer: **one schedule, owned by the entry (the driver).** A module is a library of funcs,
procs, systems, and devices; if every module shipped a `#schedule`, importing two of them would mean
two competing loops with no defined interleaving — there'd be no single timeline, and "deterministic"
would be a lie. So a module that contains a `#schedule` should be a *compile error* (the rule "one
schedule per driver," enforced semantically). Determinism is the reason you need to enforce it, and
it's cheap to enforce: it's a count.

**Q: Then how does a physics module get its `step` to run every tick?**
It doesn't *schedule* — it *contributes*. A module exposes systems (`physics.step`), and the driver
places them in its schedule. The contribution is **opt-in by the importer**, which keeps the timeline
legible: you can read one `#schedule` and know everything that runs. (A registration mechanism —
"modules may *offer* systems, the driver *accepts* them" — is the principled middle ground if explicit
listing gets tedious; the key invariant is that the driver's schedule is the single source of truth.)

**Q: What about devices?**
Same shape, plus lifecycle. A device (`gfx`) has setup/teardown that must bracket the user's work
(open the window before drawing, present after). It shouldn't own the loop, but it has phases the
runtime must honor. Proposal: a device *declares* lifecycle systems (init/teardown/present), and the
**runtime weaves them at the schedule's seams** (see the last question) — the user schedules
`draw`; the runtime ensures `present` runs at the frame boundary.

**Q: What if I want a specific loop (60 Hz game loop, an accept loop)?**
Loops are first-class schedule entries: `loop serve;`, or a grouped tick `loop { input; sim; render; }`.
A *rate* ("60 Hz") is a property of a `loop` entry, not a new construct. Genuinely *independent*
concurrent loops (a render loop and a network loop on different timelines) are a different beast —
that's concurrency, i.e. multiple timelines, and it should be an explicit, backend-aware feature, not
something you get by accident from two `#schedule`s. The default stays: one timeline, loops nested
inside it.

**Q: Can the runtime inject systems into the schedule?**
Yes — and this is the most interesting one, because the answer is **"at seams, principled, never
arbitrary."** The user's `#schedule` is the spine; the runtime is allowed to weave its own work into
the *gaps between stages*: the structural `insert`/`delete` **command-buffer flush** between `run`s
(already true today), device **present/flush** at the frame boundary, device **init/teardown** at
program start/end, hot-reload bookkeeping. The discipline: the runtime injects only at the
**schedule/stage seam**, so the user's timeline stays the readable skeleton and the injected work is
predictable (it happens *between* your stages, never inside them). What the runtime must *not* do is
inject a system whose effects interleave unpredictably with user stages — that would break the single
deterministic timeline the whole model rests on.

So the through-line for all five questions is the same invariant: **one readable, deterministic
timeline.** Modules contribute to it, devices bracket it, the runtime weaves at its seams — but
nobody gets a second one for free. Whether that invariant should ever be *relaxed* (true concurrency,
multiple timelines) is the real open question, and the honest answer today is "not until something
forces it."

## 10. Isn't this just the IO monad? (yes, and:)

Worth saying plainly, because a careful reader sees it immediately: **`Eff(T…)` is essentially
`IO a`, and yes, this is function coloring.** And the coloring is even-handed — it is *not* that
`proc` is "colored" and `func` is the neutral default. The color lives in the **types**, on both
sides: a `func … -> Eff(…)` is a *colored* function — its signature announces it traffics in effects,
exactly like `Int -> IO a` in Haskell — even though running it is pure. So there are really three
shades: a pure `func … -> T`, an effect-building `func … -> Eff`, and an effect-running
`proc`/`system`, with `Eff` the color threading through all of them. The need to *run* an effect then
infects outward to the nearest impure leaf, exactly as `IO` infects up to `main`. Same coloring, same
infection, same effects-as-values. We are not escaping the IO monad; we are constraining it.

What's different is *not* the absence of coloring — it's two constraints layered on top, each of which
earns its keep only in a no-GC systems language:

1. **The effectful color can't nest.** Haskell's `IO` composes to arbitrary depth — `IO` bound to
   `IO` bound to `IO`, forever. arche has no such construct: a proc has no way to spell a call to
   another proc, so the effect spine is depth-1 by construction — not forbidden by a rule, just
   unspellable. What makes the whole-program stack bound *provable* is static dispatch and
   monomorphization (a non-recursive deeper spine would be computable too); what depth-1 adds is that
   the bound is *trivial* — `max(system) + max(proc) + max(extern)`, by inspection. `IO`, whose spine
   nests to arbitrary depth, gives you neither the triviality nor (without a totality checker) the
   bound.
2. **The value layer is a free *applicative*, not a monad.** In Haskell you can write
   `eff1 >>= \x -> eff2 x` as a value, anywhere. In arche you cannot: `Eff` composes only *statically*
   (no runtime value flows between pieces at the value level). Result-dependent sequencing exists only
   imperatively, inside one flat proc. So arche's coloring is in fact *stricter* than `IO`'s —
   a pure func can't even consume an effect's result; it can only build the `Eff` and hand it up.

Two mechanism differences sit on top (not coloring): dispatch is **static and swappable** (the system
picks the executor at compile time — the testability story; `IO` is one fixed RTS interpreter), and
values are **bounded and heap-free** (`IO` actions and transformer stacks allocate).

So the honest pitch is *not* "we avoided monadic coloring." It is: **this is the IO-value idea,
deliberately constrained to be flat and heap-free, so a language with no GC and a real stack budget
can prove things about it** — a bounded stack, static dispatch, zero allocation — that Haskell's `IO`
cannot. If you don't want those guarantees, this is strictly *worse* than `IO`: less expressive, no
value-level `bind`. The coloring is the price; flatness, provability, and zero runtime machinery are
what it buys.

## 11. Related work, briefly (for the academics in the room)

- **Algebraic effects (Koka, Eff, OCaml 5):** we keep the vocabulary, drop the dynamic handler stack
  and continuations. Our dispatch is static and local — closer to dependency injection.
- **Free applicative / free monad:** a column of `Eff` values built by pure code and drained by one
  interpreter *is* a free applicative; the run-it-inline-in-a-proc tail is the controlled climb to the
  monadic rung for result-dependent cases.
- **Call-by-push-value:** `func` (value) vs `proc` (computation) is the CBPV distinction, drawn as
  kinds.
- **Bevy `Commands`:** the deferred structural command buffer flushed at a barrier — which arche ships
  — generalized here from structural ops to all effects.

## 12. The one-paragraph version

Make effects *values* that pure functions build and one impure leaf runs; give a procedure no way to
call a procedure so the effect graph is flat by construction; let a composer thread results between leaves;
run it all on one deterministic timeline. You get flat, explicit, heap-free, testable, data-oriented
effects with a stack bound you can prove — not by inventing machinery, but by noticing that arche's
no-heap, static-pool, column-shaped world already wanted to work this way.
