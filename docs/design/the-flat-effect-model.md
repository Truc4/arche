# The Flat Effect Model

### effects as values, procedures as leaves — side effects in arche without monads or algebraic-effect machinery

> **Status:** design exploration, not settled law. The schedule (§9) is now mostly decided — systems
> stay flat, the schedule is a derived minimal construct — with the genuinely-open parts (hierarchy/rates,
> reusable system-sequences, true concurrency) marked as such.

## Abstract

Most languages model side effects with heavy machinery: monad transformers, or algebraic effects
with a runtime handler stack and first-class continuations. arche needs neither. It models effects
with **three kinds** (`func`, `proc`, `system`) and **one primitive**: an effect is a *bounded value*
that a pure function builds and an impure leaf runs. Flatness isn't even a rule you enforce — a proc
simply has no way to call a proc, so the effect call graph is depth-1 by construction.

The surprise is that this isn't a sacrifice. The model is **flat**, **explicit** (no late-bound jump to
chase), **heap-free** (effects are fixed-size descriptors), **data-oriented** (effects compose into
columns and drain in a fanned kernel), and **statically dispatched** (a compile-time vtable, not a
runtime search). Every one of those falls *out of* arche's existing constraints — no heap, static pools,
ECS columns — rather than fighting them.

This document explains what the model is, why each piece exists, and why it's specifically good in
arche. It ends with the schedule — the timeline that runs it all.

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
| `system` | the composer | procs, funcs, maps, full control flow | **call a system** | dispatched by the runtime (via the `Schedule`) — never called |

And the keystone — the thing that makes the whole shape work:

> **A pure `func` can build an effect as a *value* and hand it out.** Building it runs nothing.

```arche
// pure: builds a description of "write this", returns it; performs no write
write_b :: func(fd: int, buf: []char) -> Eff(int, int) { return fwrite(fd, buf); }   // extern under-applied by its out-slots
```

`Eff(int, int)` is the type of a **not-yet-run effect**: a bounded value standing for "an effect that,
when run, yields these results" — here the two out-slots `(n, err)` of `fwrite`. Think Haskell's `IO a`,
adapted to arche's multi-out-slot convention. There is **no special keyword in either direction** — an
`Eff` is just an extern *under-applied* by its out-slots (saturation), and you **run** it by *supplying*
those out-slots:

```arche
eff := write_b(fd, line);   // PURE: build the effect value, bind it like any other value
eff(n:, err:);              // IMPURE: run it — the out-slots come from running, not from the func
```

That two-line split is the entire model in miniature:

- **the `func` world** is the *value* world. Effects-as-data live here. They nest, compose, get passed
  around, get chosen conditionally, get stored — all pure.
- **`eff(out:)`** is the single impure act: calling an `Eff` with out-slots turns the inert value into
  a real call and binds its results. Out-params belong to *running* an effect, never to a func — which
  is also why a pure func can never have effect out-params, only an `Eff` return.

Because reuse lives entirely in the `func` world, **a proc never needs to call a proc — and the
language gives it no way to.** You can't factor a shared effectful step into a sub-proc; you factor it
into a func that builds the `Eff`. So flatness isn't a checked rule with an error message — it's
**unspellable**: an effect is inert data (it can't run anything), and a proc has no syntax to invoke
another proc. The one back door — under-apply a proc to "build an `Eff`" from it, then run that from
another proc — is closed by construction: **only an `extern` is inert when under-applied.** An extern
minus its out-slots is just an opcode plus inputs — a value; a *proc* minus its out-slots is a
*suspended computation*, exactly the runnable thunk the value world refuses to contain. So
under-applying a proc is an **error**, not an `Eff` — there is no expression anywhere that turns a proc
into one. (An optional `wrap` keyword may mark a build site for emphasis — `return wrap fwrite(…)` ≡
`return fwrite(…)` — but it is never required and never changes what's wrappable.) The proc shrinks to a
thin imperative shell: *run these effects, react, report via out-params.*

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

### The convenience layer: stdlib procs become funcs→`Eff`

This has a sharp, practical consequence the moment you read the real stdlib. Today `net.recv`,
`http.respond`, `io.read`, `os.now_ms` are **procs** — convenience wrappers over the true leaves
(`net_recv` / `net_send` externs; `os.syscall`). Under the ban a func may **not** wrap a proc, so these
can't be lifted to `Eff` as written: wrapping `http.respond` would be the proc→proc loophole wearing a
func hat. The fix isn't to keep them procs — it's to rewrite the convenience layer itself as
**funcs→`Eff`** that wrap the *real* primitive, never another proc. Almost every wrapper is exactly
that shape: a thin builder around one primitive.

> **The settled recipe — the lib builds the raw effect as a `func → Eff`; the cook rides inside it.** A
> wrapper is `func(args…) -> Eff(<the primitive's out-slots>)` — a thin builder around one primitive, never
> another proc. The pure post-processing the old proc did (`i64→fd` cast, `buf[0:r]`, the ms arithmetic)
> rides **inside** the `Eff` via `|>` (fmap), so the wrapper stays a func and the call site is one line.

This needs no `Result` type and no generic sum: **the result/status is just an out-slot.** A read returns
its byte count (or negative errno) as the `Eff`'s out-slot; the caller branches on it. Fallibility is *data
in an out-slot*, reacted to in the proc/system — the model's failure-as-data (§7).

### The buffer model — a kernel-written buffer is the caller-allocated OUT param

A syscall that *fills* a buffer (`read`'s `buf`, `clock`'s `[2]i64` timespec) does **not** take it as an
in-arg: an in-param can't supply a writable allocation — that is the **out param's** job. The buffer is the
caller-allocated **OUT** param.

- In the out-list, **`name: [N]T` ALLOCATES** a fresh buffer caller-side; **`name`** (no `:`) writes an
  **existing** variable or pool column.
- The in-list still carries the buffer's positional slot (the C ABI passes the pointer there), written
  **`_`** at the call site — a C-ABI shadow that supplies no value. The same name in both lists marks the
  param **in-out** (kernel reads+writes in place); the shadow surfaces as the W0012 lint.

```arche
read :: func(fd: int, len: int) -> Eff([]char, i64) {
  return sys_read(fd, _, len);            // `_` is the C-ABI in-slot; the buffer is the OUT
}
io.read(fd, 256)(buf: [256]char, n:);  data := buf[0: n];  // fresh local: `:` allocates the OUT
io.read(fd, n)(cb, k:);                                    // or write an existing/queried buffer (no `:`)
```

The mutability is **declared, not special-cased**: a buffer the kernel writes is an in-out param of a typed
`@syscall(N)` extern; a buffer it only reads is a plain-in borrow. Buffers may **not** be passed to the
scalars-only generic `syscall` (it can't declare the mutability) — closing the hole of a syscall scribbling
through a read-only borrow.

### fmap-over-buffer — the cook rides inside the Eff

`|>` applies a pure finalizer that the *run* applies to the effect's result — not only a scalar return but a
kernel-written **OUT buffer**. When the finalizer folds a buffer, the buffer is **run-internal scratch** the
caller never sees; the finalizer's result is what's bound.

```arche
clock_mono :: func() -> Eff([2]i64) { return sys_clock(1, _); }      // single out-slot: the timespec
ms_of      :: func(ts: []i64) -> i64 { return ts[0]*1000 + ts[1]/1000000; }
now_ms     :: func() -> Eff(i64) { return clock_mono() |> ms_of; }   // the cook rides INSIDE the Eff
now := 0;  os.now_ms()(now:);                                        // common case — one line, no buffer
open :: func(path: []char, flags: int) -> Eff(int) { return sys_open(path, flags, 0) |> fd_of; } // scalar reshape
```

This is why the convenience the old proc carried can live in the *lib* as a func, not smeared caller-side:
the builder under-applies the extern, `|>` attaches the pure cook, the run does both. The wrapped leaf is
always the genuine primitive, so the spine stays depth-1 and no ban is touched.

### Classifying a stdlib proc (the conversion decision tree)

1. **Pure result, no buffer** → `func → Eff(scalar)`, optional `|> fin` scalar reshape (`os.open`, `os.close`).
2. **Kernel-written buffer** → the buffer is the caller-allocated OUT param (`io.read`, `net.recv`); fold it
   to a value with `|> fin` (`os.now_ms`), or bind the buffer and cook at the call site.
3. **Caller-built input buffer** → build it element-by-element in the func, pass it (`os.sleep_ms` builds a
   timespec, then `nanosleep(req)`). Input builds are free funcs — the buffer is live *before* the run.
4. **Result-dependent (monadic) sequencing** → STAYS a proc (the one exception, below).

**The one genuine exception is result-dependent convenience.** A wrapper whose *internal* sequencing is
monadic — `io.fread_line` (read char-by-char until `\n`), a retry loop, a framed "read a length then read
that many bytes" — cannot be a func/`Eff` (that needs a free monad: continuations, allocation, unbounded).
It is the **only** stdlib wrapper that stays a `proc`: the lib exposes the *leaf* as a func→`Eff`
(`read1 -> Eff(char,int)`) and the monadic loop lives in the consuming proc/system — the cost is the loop
isn't shared. The principled long-term home is a fourth construct (the `routine`, see §9).

## 5. The applicative/imperative line (the one subtlety worth memorizing)

There are two kinds of effect composition, and arche puts them in two different places.

**Static composition → funcs.** When the *shape* of the effect graph is known up front — "this write,
then a newline" — a func can build the whole thing as one `Eff`. No runtime value flows between the
pieces. This is the **applicative** rung: structure known before execution, which is exactly why it's
safe to inspect, reorder, batch, and run in parallel.

The two composition primitives a func has are `|>` (fmap — reshape one effect's result) and **`zip` (the
applicative product — combine N INDEPENDENT effects)**. `zip(e1, …, eN)` runs each effect and yields one
`Eff` whose out-slots are the concatenation of all of theirs; the run binds them positionally, or a
`zip(…) |> fin` folds **all** the slots through one finalizer (the single-out `|>` is the N=1 case).
Independence is the whole point — no out-slot feeds another effect's *shape*, so order is free:

```arche
// gfx: a window's framebuffer + dims as one Eff — three independent backend reads, no combiner
frame :: func(h: window) -> Eff([]int, int, int) { return zip(gfx_be_frame(h), gfx_be_w(h), gfx_be_h(h)); }
gfx.frame(handle)(px:, w:, h:);                                  // bind all three out-slots at once

// argv-shape: combine two independent reads with a cook
argv :: func(i: int) -> Eff([]char) { return zip(os_argv(i), os_argv_len(i)) |> slice_of; }
```

(`zip(…) |> fin` is supported for scalar slots/returns today; a *slice* slot/return — a bare-pointer +
length recombine like `os.argv`'s `raw[0:n]` — fights checked-slice safety and is not yet supported, so
`os.argv` itself stays a proc for now. See "Deferred" below.)

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
  value. The composition primitives are **`zip`** (the applicative product — combine N *independent*
  effects, yielding the union of their out-slots), **`seq`** (run-then-discard ordering), and **`|>`**
  (fmap a pure cook over the result). When run, the value performs its effects and yields its out-slots.
  Crucially this adds **breadth, not depth** — the run unfolds to `system → proc → {extern, extern, …}`,
  still depth-1, because the composition happened in the value world (where nesting is free) and flattens
  to terminal externs with no proc in between.

- **No — a result-dependent series cannot be an `Eff`.** The moment a later effect's *input* is an
  earlier effect's *output* (`read` a length, then `read` that many bytes), you'd need a free *monad*
  carrying runtime continuations — allocation, unbounded depth, exactly what arche refuses. So it stays
  imperative, inside one proc (the `handle_msg` example above).

- **No — a series of *procs* never collapses into a value at all.** A proc isn't a value (a proc minus
  its out-slots is a suspended computation, not inert data), so nothing ingests several procs and emits
  one `Eff`. What *sequences* procs — threading out-slots, branching on results — is a **`system`**, and
  it produces a schedule step, not a value. There is deliberately no path from "several procs" to "one
  value": that path is precisely what would make procs first-class and collapse the flatness.

### Diagnostics the model enforces

- **E0223 `extern_multi_out`** — a `#foreign` proc may declare at most ONE out-*only* out-param (it maps to
  the C return value); additional kernel-written outputs must be **in-out buffer** params (a name in both
  the in- and out-list). More than one out-only out-param used to silently miscompile; it is now rejected.
- **W0012 `inout_param_shadow`** — surfaces the C-ABI in-out shadow (the in-list buffer entry written `_`),
  so the "same name in both lists" is visible rather than silent. Permitted for `#foreign`/`@syscall`.
- **W0029 `pool_index_outside_query`** — a pool column reached by hand (`Pool.col[i]` *or* `Pool.col[a:b]`)
  outside a query/system fan; pool values must come from a query. (The slice form catches a pool-column
  buffer filled as a read OUT target outside a query.)

### Deferred — the honest edges

- **`zip(…) |> fin` with a slice slot or slice return** is unsupported: a bare-pointer + length recombine
  (`os.argv`: `raw[0:n]`) has no sound length to hand the cook's slice param, and fights checked-slice
  safety (`!undefined` is forbidden). Scalar slots/returns work; `os.argv` therefore stays a **proc**.
- **Result-dependent (monadic) sequencing** stays in a proc — the applicative/imperative line above; the
  principled long-term home is the `routine` construct (§9).
- **Higher-order systems** (a system parameterized by a body) are deliberately absent: that would hoist
  iteration into a value and reintroduce the depth the model exists to forbid. Repeated `each (query {…})`
  fan headers across draw systems are structural skeleton, not duplication to abstract away.

### The free selective, fully compile-time — what it buys, what it costs

The value layer is a **free selective** (`pure`, `zip`, `seq`, `|>`, plus `ifS`/`whenS`) — the middle
rung between applicative and monad (Mokhov et al., ICFP 2019). It can **conditionally execute** a
statically-known set of effects — gate an effect on a *pure* value (`whenS(cond, eff)` runs `eff` iff
`cond`; `ifS(cond, a, b)` picks one) — but it has **no value-level `bind`**: a later effect's *shape* can
never depend on an earlier's *runtime result*. And it is a **fully compile-time abstraction** — with no
heap, an `Eff` is never a runtime tree of nodes the way Haskell's `IO`/free structures are. The compiler
**folds the `Eff` graph at compile time** and lowers it to flat, straight-line leaf calls (a selective
node lowers to a guarded branch, `if (cond) { … }`); the tree lives in the compiler, never the program.
That single fact is why the combinators are **compiler intrinsics, not library functions you write**:
there is no runtime structure for an ordinary `func` to build or walk, and arche has neither the heap nor
the generics to express `zip :: Eff a -> Eff b -> Eff (a, b)` as user code. (Haskell makes all of these
ordinary library code precisely *because* it has both; arche's own `Schedule` — combinators as `func`s
over a `sum`, folded at `#run` — shows the CTFE middle path exists, so this is an implementation choice,
not a law.)

**What it buys:**
- **Heap-free and interpreter-free.** No runtime effect tree, no walk, no RTS — an `Eff` compiles
  *away* into the leaf calls it described.
- **A statically-known effect set — even with conditionals.** The whole graph is known before anything
  runs; a selective gates *which* effects execute but never hides *what* effects *could* (the scheduler
  over-approximates: "this might run `fail`"). So the compiler can still batch a column into one kernel,
  reorder `zip`'s independent legs, and fuse `|>` cooks. Inspectability survives the conditional.
- **A provably bounded stack** (§6): the spine is `system → proc → extern`, depth-1, computable.

**What it costs:**
- **No value-level `bind`** — the one real limit. Result-dependent sequencing (a later effect's *shape*
  from an earlier's *result*) can't be a value; it runs imperatively in a proc (§5). The selective rung
  buys conditional *execution* of a static set; `bind` would make the set itself depend on runtime
  values — unbounded, heap-needing — the line arche won't cross. (A self-recursive selective builder is
  rejected for exactly this reason: it would be a free monad.)

The through-line: **the `Eff` stays a fully static, compile-time-foldable value with a statically-known
effect set.** Conditional *execution* (selective) is in; making the effect *set* depend on a runtime
value (`bind`) is out — pushed to the proc/system, or left unbuilt until a concrete case forces a
stronger algebra. That discipline is what makes "effects as values" cost zero runtime machinery.

(`assert` rides on this: `assert(c, m) = whenS(c == 0, fail(m))` — a pure `func -> Eff()`, run with `()`.
Today the **guard form** is wired — `whenS`, and `ifS` with a `pure()` else; value-producing `ifS`, where
both arms yield a non-empty `Eff(T)` and the run binds the chosen result, is the next step.)

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
write_b :: func(fd: int, buf: []char) -> Eff(int, int) { return fwrite(fd, buf); }  // under-applied extern = Eff

write_row :: each query { price, quantity, price_bucket, ok } (fd: int) {   // effect leaf, fanned
  eff := write_b(fd, fmt_row(price, quantity, price_bucket));
  eff(n:, err:);
  ok = (err == 0);                                                          // per-row failure as data
}

write_out :: system {                       // composer: open, fan, close; threads fd
  open_csv("…/out.csv")(fd:, ok:);
  if ok { run write_row(fd); close_csv(fd); }
}

#run once(seq({ run(load), run(bucket), run(write_out) }))   // a one-shot ETL: run once, then halt
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

## 9. The schedule: scheduling logic as a value

> This section is the **effect-model framing** of the schedule — *why* the loop is a value (the same move
> that reified effects). The **operational spec** — the `Schedule` sum, the compile-time fold, the
> `#run`-collection rule, the pacing recipe, and state-in-pools — lives in
> [scheduling.md](scheduling.md). Where they overlap, scheduling.md is the current source of truth.

There is no `main`. A program is its declarations plus one **`Schedule`** value, and the **runtime**
executes that value. The runtime is the loop and the apex of the spine — and it is not a kind you write.
A `main`-as-driver-proc cannot exist: a proc may not reach a proc, so a top-level proc could never run the
program's effectful work without being the very thing the model forbids. The roles `main` used to play
split cleanly and each becomes a value the runtime runs: the **entry** is the runtime, **setup** is a
`Schedule`, the **loop** is a `Schedule`, **per-tick work** is a `Schedule`.

**The schedule is a value, built the same way effects are.** §3 reified an effect as a value (an extern
under-applied by its out-slots) so a proc never has to call a proc. Scheduling reifies the *loop* the same
way: the imperative driver-loop you can't write — it would be a proc reaching procs — becomes a `Schedule`
value you compose, and the runtime runs it. Effects → `Eff`; pacing and dispatch → `Schedule`. One move,
both halves.

**Systems stay flat: a `system` may not call a `system`.** This is the `proc→proc` ban one rung up, and
it holds for the same reason — *legibility*, not the stack bound (a non-recursive deeper spine is
computable too, §6; depth-1 only makes the bound a one-liner). Read the schedule and you have the
complete inventory of effect-bearing units; let systems call systems and the effect set hides inside a
call graph nobody can see at a glance — the exact rot §1 opens by condemning. The survey is blunt: 4 of 5
frameworks forbid system-calls-system outright; the one that allows it (Unity DOTS `ComponentSystemGroup`)
had to invent a separate composer kind and pays with an unbounded depth-first spine — the precise
anti-arche choice. **Composition lives in the schedule, never in inter-system calls.**

**The composition spine has four levels, capped at both ends.** Each level composes only the level
directly beneath it, and nothing composes its own level:

```
runtime  ── runs → the Schedule, which dispatches → systems   ← apex: the loop, NOT a kind
  system ── composes → procs + maps                            ← the composer (dispatched, never called)
    proc ── composes → externs                                 ← a flat effect leaf (never a proc)
      extern ── the atom: one syscall                          ← floor: the primitive boundary (§4)
```

The "no same-level" rule *is* the ban, generalized: **proc ✗ proc** forces effect-composition up into
systems; **system ✗ system** forces system-ordering up into the `Schedule`. It terminates instead of
towering because the apex is the **runtime** — not a kind you can ask "what composes *it*" — and the floor
is the **extern**. Four fixed levels; no `driver`, no super-system. (Orthogonal to the spine sits the value
plane: `func` builds `Eff` and `Schedule` values, `map` is the data-parallel kernel.)

**The core is small — the runtime's ABI.** The runtime natively interprets a fixed set of `Schedule`
constructors — `seq([]Schedule)`, `par([]Schedule)`, `loop(Schedule)`, `when(func()->bool, Schedule)`,
`halt` — and a **leaf is a bare system/map/each name** (`#run seq({ boot, render })`). The earlier `run(s)`
leaf constructor is **retired**: a system/map is scheduled by name, never wrapped. The whole `Schedule` is a
compile-time-constant, heap-free tree the runtime walks, allocating nothing. (Full constructor semantics,
the compile-time fold, and the `#run`-collection rule are in [scheduling.md](scheduling.md).)

**Structure combinators are `func`s you write over the core — not a DSL, not a stdlib.** Only `once` and
`forever` ship (in `core.arche`); any other *structural* combinator is yours to write as a func returning
`Schedule`:

```arche
once    :: func(s: Schedule) -> Schedule { return seq({ s, halt }); }
forever :: func(s: Schedule) -> Schedule { return loop(s); }
```

There is deliberately **no `at_hz`/`every`/`catch_up`** — because pacing is not a `Schedule` value at all.
The schedule loops dumbly; *timing* is work a system does (read `os.now_ms`, decide whether to
`os.sleep_ms`). So the earlier fear of a bolted-on schedule DSL (`loop`/`@fixed`/`@await`) dissolves from
both ends: structure is ordinary `func`-built values, and pacing is ordinary system work — neither is a
second language.

**A leaf is a bare system name.** `#run seq({ draw, present })` dispatches `draw` then `present` by their
compile-time identities — there is **no `run(…)` wrapper** (the earlier explicit-`run` leaf was retired in
favor of this bare-name lift; `map`/`each` are kinds of systems, scheduled the same way).
Sub-schedules don't need lifting either — they already are `Schedule`s — which is what lets schedules nest
for free.

**Scheduling structures; systems do.** A `Schedule` is a pure value: pure structure (`seq`/`par`/`loop`).
It performs no effect and the runtime owns no state. There is **no `World`** — an earlier draft gave
predicates a `func(World) -> bool` over a runtime `World` value, but that was a mistake twice over: "World"
is the ECS word for the pool universe (which arche already *is*), and the things it bundled each have a
concrete home — the **clock** is a syscall, **input** is a device writing an event pool, a **tick count**
is a `[1]` singleton a system bumps. **The schedule does not gate on program state at all** — there is no
`when`-over-state pass; the runtime walks the static tree and dispatches, reading no pool to decide what
runs. The `when` constructor is reserved but unwired; its one plausible future use is **loop termination**
(stop the loop on a quit flag so teardown runs after) — a **deferred TODO**, with `os.exit` in a system
ending the program today. **Timing is not a schedule concern either** — the schedule loops dumbly; a system
reads the delta (`os.now_ms`) and decides whether to wait (`os.sleep_ms`). Effects, including pacing,
happen in the dispatched systems; the schedule is just *what* runs and *in what order*.

**Parallelism is derived from data; order is what you wrote.** Systems already declare which pool columns
they read and write (the barrier logic needs it). `seq` is explicit order — run these in this sequence.
`par` is a *request* honored only within those data dependencies: two systems that touch disjoint columns
run concurrently, a writer orders before its readers, and a conflict serializes rather than races. So you
compose the timeline you want and the runtime parallelizes inside it. (Bevy leaves conflicting systems'
relative order unspecified — a footgun. arche's written `Schedule` plus derived-but-stable concurrency is
the Flecs-phase / specs-named-dep family, not parallelize-and-hope.)

**Systems compose through data across barriers, never the stack** — and every piece already exists:
schedule ordering (temporal composition), the deferred `insert`/`delete` command buffer flushed at the
seam (Bevy `ApplyDeferred`, Unity `EntityCommandBufferSystem`, Legion `.flush()`, specs `maintain()` —
universal), event pools drained next tick, and singleton (`[1]`-pool) state. `input→sim→draw` is three
flat systems the schedule sequences, data flowing through pools — not a call between them.

```arche
[256]InputEvent;   [256]RenderCmd;            // event channels, drained next tick
[1]Sim;   Sim :: arche { /* sim state */ }    // a [1] singleton — NOT a "World" (that's all the pools)

gather :: system { /* run input-device leaves → write InputEvent rows */ }
step   :: system { /* drain InputEvent, mutate Sim, write RenderCmd rows */ }
paint  :: system { /* drain RenderCmd → run draw-device leaves */ }

#run forever(seq({ gather, step, paint }))   // ONE tick, sequenced; barrier at each seam (leaves are bare names)
```

**`#run` names the program's one `Schedule` value for the runtime to execute.** There is exactly one per
program (a module that ships a `#run` is a compile error — two timelines means no timeline). **Systems are
global the way pools and types are** — a system has one name in the program's vocabulary, and the schedule
names it bare (`draw`, never `gfx.draw`). A device *contributes* systems to that global namespace; it does
not own a callable surface anyone reaches into.

**The schedule expresses STRUCTURE; timing and conditions are not its job.** Only the core
(`seq`/`par`/`loop`/`when`/`halt` + `once`/`forever`) is provided. *Structural* shapes are `Schedule`
values; *timing* is work a system does:

| concern | how |
|---|---|
| tick (default) | `forever(serve)` — a schedule value (core) |
| startup-then-loop | `seq({ boot, forever(serve) })` — a schedule value (core) |
| nested / sub-step / rollback | embed a `Schedule` in a `Schedule` — values nest (core) |
| time / Hz, fixed-step + catch-up | a **system** does all the timing (read `os.now_ms`, accumulate a bank, let the sim map run while behind, sleep the remainder when caught up); the schedule just `loop`s and re-runs the system to drain catch-up. No schedule condition — timing is wholly system work |
| event-gated, state-gated | **not a schedule feature** — gate inside a system's body (`if`), or end the loop with `os.exit`. The schedule reads no program state. (A schedule-level loop-end hook is a deferred TODO.) |
| CPU / cores | `forever(par({ physics, ai }))` — `par`-as-true-concurrency is **open** |

**Setup** is just a `boot` entry *before* the loop
in the `seq` — no separate "startup phase" construct, it simply runs once because a `seq` entry runs once.
(`once(s)` is the *one-shot program* wrapper — `seq({ s, halt })`, run-then-exit — not a setup prefix; using
it before a `forever` would `halt` the program before the loop ever starts.) **Nested
ticks / sub-schedules** (sub-stepped physics, a rollback re-sim, a nested mini-game) are a `Schedule` inside
a `Schedule` — no new keyword, because values nest. And a **reusable named sequence of systems** is just a
`func` returning a `Schedule` — the "reuse residue" dissolves too.

```arche
#run seq({ boot, forever(serve) })   // setup once, then loop — one value (leaves are bare names)
```

There is no `tick` keyword. Advancing the schedule is the runtime's job, not a statement a program writes —
which is the whole reason there is no driver loop and no `main` to put one in.

**Devices bracket the timeline; the runtime weaves lifecycle at seams.** A device declares
init/present/teardown systems; the runtime runs them at schedule seams (present at the frame boundary,
init/teardown at program start/end) — the same seam machinery as the command flush, never interleaved
*inside* a stage. The user schedules `draw`; the runtime guarantees `present` after it. (Bevy
`Plugin::build`, Flecs modules, Unity root groups.) A module likewise *contributes* systems the schedule
places; it never owns a timeline.

**A device's public surface is systems, pools, and types — never procs or funcs.** This is the ECS
discipline taken seriously: a device *is* its data (pools/components, plus singleton `[1]`-pools for
boundary state like a window handle) and the **systems** that transform it. Procs, funcs, and the foreign
externs underneath are *private* device internals — a system runs them, but they are not the interface.
The **schedule** places a device's systems; nothing *calls* a device proc or threads external context by
hand. That keeps the timeline complete: every device effect appears in the schedule, nothing hides behind
an imperative device call. (This is exactly Bevy plugins and Flecs modules — a device adds
systems + components to the global namespace, and the schedule wires them; it does not export a bag of
callable functions.)

The payoff is concrete. arche-rpg today exports `draw_ball` as a **proc** and the program hand-loops over
entities calling it (`for i … { draw_ball(win, pos.x[i], …) }`) — because a system "can't take the window
handle." Make the window a singleton pool and `draw` becomes a **system** over `(pos, color)` + `Window`:
the manual loop vanishes, drawing fans over the column like any other kernel, and the schedule just names
`draw`. The thing that *forced* the proc — threading external context — dissolves once context is a
singleton the system reads. Systems are global the way pools and types are; a device contributes to
that global vocabulary, and the schedule is where it all gets ordered.

**Why a singleton and not an argument?** Because a system taking no params is the uniform state model, not a
map limitation worked around. All concrete state in arche lives in pools; kernels and systems read that
ambient state directly (`Config.gravity[0]`), the same way a map reads its columns. Threading the window in
by value would make it the *one* piece of state passed by hand while everything else is read in place — the
inconsistency, not the fix. You pass a thing when the callee doesn't own it and the caller picks the
instance; the window fails that test both ways (the device owns it, and there is one). If there were ever
*two* windows that is a real pool with rows — a system iterates them or a query selects one — but the
singleton case is genuine ambient state.

**The handle is private; only properties surface.** Split the window's two roles. The **handle/connection**
(the X11 display pointer, the fd, the plumbing) is a device-*internal* singleton — never named by user code,
read only by gfx's own systems (`present`, `clear`). It sits behind the public systems exactly as the proc
and foreign externs do. The window's **properties** (width, height) surface *only when user code reads them*:
gfx publishes a read-only `Screen { width, height }` singleton that a layout map reads like any other ambient
state. So the user never sees, names, or passes the handle — the discomfort of "manually passing a singleton"
is answered by *not exposing the handle at all*, not by passing it some other way.

### Still genuinely open

Reifying the schedule as a value closed three problems that were open under the old directive form:
**nested ticks / sub-schedules** are a `Schedule` inside a `Schedule`; a **reusable named sequence** is a
`func` returning a `Schedule`; **startup** is a `boot` entry before the loop. None needs a new construct or relaxes
the system ban. What remains:

- **A schedule-level loop-end hook (deferred TODO).** `Schedule`-as-a-recursive-value, the fold, `#run`,
  the core constructors, and dumb `loop`-of-systems all shipped, and **pacing is resolved**: it is not a
  schedule concern — a system reads `os.now_ms` and decides whether to `os.sleep_ms` (so there is no clock
  condition, no `World`, no pacing primitive). **Runtime conditions over program state are scrapped** — the
  schedule reads no pool/global to gate dispatch; conditional work lives inside a system's body (`if`). The
  *one* place a runtime condition might still earn itself is **loop termination** — stopping the loop on a
  quit flag so teardown systems run *after* it (`loop`-until / `when(should_quit, halt)`), instead of the
  hard `os.exit`-in-a-system used today. That is a **deferred TODO**, not being built now.
- **Truly independent timelines.** `par` is cooperative concurrency *within one tick*, honored within data
  dependencies. Two genuinely independent loops (a render loop ‖ a net loop, each with its own cadence)
  stay out of scope — explicit, backend-aware, not until something forces it.
- **A new dispatch primitive.** Composing the core (`seq`/`par`/`loop`) with system-body logic covers every
  pacing that *derives*. A fundamentally new execution semantics (preemption, I/O-completion
  dispatch) would be a new runtime primitive, not a library `func` — and that needs runtime support.

### A second, unrelated open problem (from §4): result-dependent reusable convenience

The convenience-layer rewrite (§4) lifts almost every stdlib wrapper to a func→`Eff`. The holdouts are the
wrappers whose own body is monadic — `io.fread_line`, a retry loop, a framed read: not a func (impure),
not an `Eff` (a free monad — allocating, unbounded), not a sub-proc (procs aren't values). The proposed fix
is a fourth construct — a **`routine`**: a proc-bodied unit the compiler splices into the caller
(monomorphized, no independent frame). Reusable *and* result-dependent, yet flattened at compile time, so
the depth-1 spine and trivial bound survive and no free monad returns (structure static, only the frame
inlined). Note the symmetry: this is the proc-level twin of the system-level phase construct above — both
are compile-time-flattened reuse, neither relaxes a runtime nesting ban. Not settled.

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
2. **The value layer is a free *selective*, not a monad.** In Haskell you can write
   `eff1 >>= \x -> eff2 x` as a value, anywhere. In arche you cannot: `Eff`'s effect *set* is fixed
   statically — `whenS`/`ifS` can conditionally *execute* a known effect (gate it on a pure value), but
   no runtime result can change *which* effects exist. Result-dependent sequencing exists only
   imperatively, inside one flat proc. So arche's coloring is in fact *stricter* than `IO`'s — a pure
   func can gate effects but can't *consume* an effect's result to choose the next; it builds the `Eff`
   and hands it up.

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
- **Free applicative / selective / monad (Mokhov, Lukyanov, Marlow, Dimino, ICFP 2019):** a column of
  `Eff` values built by pure code and drained by one interpreter *is* a free applicative; `whenS`/`ifS`
  lift it to the **selective** rung (conditional execution of a static effect set); the
  run-it-inline-in-a-proc tail is the controlled climb to the monadic rung for result-dependent cases.
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
