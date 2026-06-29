# The Arche language

A reference for Arche's memory model, type map, declarations, and the
`func` / `system` / `map` split. For tooling (CLI, editor, build) see
[tooling.md](tooling.md); for benchmarks see [performance.md](performance.md);
for doc comments and doctests see [DOCTESTS.md](DOCTESTS.md).

## Contents

- [Memory model: static allocation only](#memory-model-static-allocation-only)
- [Types and declarations](#types-and-declarations)
- [Numeric model](#numeric-model)
- [Archetypes](#archetypes)
- [Array-oriented operations](#array-oriented-operations)
- [Indexing](#indexing)
- [Arrays and slices: `[N]T` values, `[]T` slices](#arrays-and-slices-nt-values-t-slices)
- [Effects: systems (`system`, `each`)](#effects-systems-system-each)
- [Maps (`map`)](#maps-map)
- [Functions (`func`)](#functions-func)
- [`func` vs `map` vs `system`](#func-vs-map-vs-system)
- [Enums and `match`](#enums-and-match)
- [Ownership: borrow, `move`, `copy`](#ownership-borrow-move-copy)
- [Foreign resources: `opaque` types](#foreign-resources-opaque-types)
- [Multiple outputs and mutate-in-place](#multiple-outputs-and-mutate-in-place)
- [Implicit loop codegen](#implicit-loop-codegen)
- [Design priorities](#design-priorities)
- [Worlds (planned)](#worlds-planned)

## Memory model: static allocation only

**Arche has NO dynamic or heap allocation.** All memory is allocated statically at
program startup. This is a core design principle, not a limitation.

- **Explicit allocation**: `[N]Archetype` declares a fixed-capacity pool for an archetype shape. There is no `static` keyword — every top-level declaration is static-lifetime, so position alone marks the storage.
- **Static storage**: All pools have program lifetime. They exist from program start to end. No deallocation during execution.
- **Fixed size**: Each pool is immutable in size. No resizing, no growing vectors. Capacity is permanent once set.
- **Implicit cleanup**: At program end, all storage is implicitly freed. Explicit deallocation is **not allowed** (and unnecessary).
- **Initial live count**: `[N]Archetype(M)` allocates capacity `N` and starts with `M` live rows; deleted slots are tracked in a free-list and reused by later inserts, eliminating fragmentation.

A pool declaration can include field initialization to set the live instances' values at allocation time:

```arche
val   :: int;
score :: float;
Counter :: arche { val, score };

[5]Counter(5) { val: 7, score: 2.5 };
```

This reserves capacity for 5 instances, starts all 5 live, and initializes the `val` and `score` columns. Uninitialized columns are zero-initialized.

**Encouraged pattern** - plan memory upfront, allocate what you need, use predictably:

```arche
active :: int;
hp     :: int;
speed  :: int;

Particle   :: arche { active };
Enemy      :: arche { hp };
Projectile :: arche { speed };

[10000]Particle(10000) { active: 0 };
[5000]Enemy(5000);
[1000]Projectile(1000);

initialize  :: map (query { active }) { active = 1; }
update_loop :: map (query { speed })  { speed = speed + 1; }

#run seq({ initialize, update_loop }) // the schedule names the work; the runtime owns the loop
```

**Why no dynamic memory?** Predictable memory usage (known at program start), zero
allocation overhead in hot loops, a fixed budget that forces clear thinking about
capacity, cache-friendly columnar layout, and no use-after-free / dangling pointers /
fragmentation.

> **"No dynamic allocation" ≠ "no dynamic archetypes."** This is a property of the *core
> language*: it bakes in no implicit heap. It is **not** a ceiling on the data model.
> **Dynamic (resizable) archetypes — the backbone of a full ECS — are planned as a later
> library layer** built on the same columnar pools, once the core matures. The core staying
> allocation-free is what lets such a layer be an explicit, opt-in library rather than a
> hidden cost in every program.

## Types and declarations

Types are **nominal** - identity is the name, not the structure. The binding form is
universal: `name : [type] (: | =) value`.

- `:=` - runtime variable, inferred type (`x := 5`)
- `::` - compile-time constant: a *type* if the right-hand side is a type, a *value const*
  if it is a literal (the kind is inferred - there is no `type` keyword in user code)
- `: T = e` - variable with an explicit type; `: T` alone zero-initializes
- `: T : e` - constant with an explicit type
- `type` is the meta-type (type-of-types), compile-time only; `x : type : T` is a local type alias

```arche
meters :: float;    // nominal type alias (distinct from any other float type)
seconds :: float;   // meters != seconds, though both back to float
MAX :: 100;         // value const (literal RHS)
x := 5;             // runtime variable
```

The short forms are **elisions of the one canonical form**, not separate rules — `x :: e` is
`x : ⟨inferred⟩ : e`, `x := e` is `x : ⟨inferred⟩ = e`, and `x : T` is `x : T = 0` (the value
elided defaults to zero). The value separator is the mutability axis: `:` binds an immutable
compile-time **definition**, `=` binds mutable **storage**. Implicit and explicit are treated
identically.

These forms work at **file scope** too: `count := 0`, `limit : int = MAX`, `buf : [8]int` are
mutable globals. A mutable global's *initial value* must be compile-time-constant (static storage
is initialized at link time — there is no startup code), but the variable is still reassignable;
a non-constant initializer is an error. Implicit `flag : int` is exactly `flag : int = 0`.

**Name visibility and order.** All top-level names — definitions *and* mutable-storage bindings —
are visible across the whole file regardless of order: a `system` may read a global declared below
it, just as it may call a `func` defined later. (Locals, by contrast, are visible only from their point
of introduction onward.)

A type alias is **zero-cost** - it erases to its backing after checking. Operators resolve
on the backing (so `meters + meters -> meters`, and mixed-alias arithmetic over the same
backing is fine), but **nominal identity is enforced at substitution boundaries**
(parameters, assignment, return): you cannot pass `meters` where `seconds` is expected.
This is the entire basis of foreign-resource safety (`file :: opaque` != `socket :: opaque`).

## Numeric model

- Base primitives: `int`, `float` (32-bit, f32 — like Jai; matches the GPU), `char` (no `bool` type)
- Comparisons produce numeric values (`0` or `1`); conditions treat `0` as false, non-zero as true

```arche
a := 3;
b := 7;
x := a < b;   // x is 0 or 1
fmt.assert(x == 1, "a < b\n")();
```

**Fixed-width integers** are always available alongside `int`: `i8`/`u8`, `i16`/`u16`,
`i32`/`u32`, `i64`/`u64`, `i128`/`u128`, and `byte` (= `u8`). `int` is an alias for `i32`;
`char` is `i8`. These map to native LLVM integers - no software emulation. Signedness is
part of the type and selects the machine operation (`sdiv`/`udiv`, signed/unsigned
compares, `sext`/`zext`).

```arche
offset: i64 = 3000000000;   // addresses past the 32-bit signed limit
offset = offset + offset;   // i64 arithmetic
count: u32 = 4000000000;    // unsigned 32-bit
b: byte = 255;
```

Convert between widths with a call-style cast (`sext`/`zext` to widen, `trunc` to narrow):

```arche
offset: i64 = 5;
small := i32(offset);   // truncate i64 -> i32
wide: i64 = i64(small); // widen i32 -> i64
fmt.assert(wide == 5, "round trip\n")();
```

Integer literals adopt the type of their context (`x: i64 = 3000000000` types the literal
as `i64`, avoiding 32-bit overflow). Operands of differing widths are widened to the larger
width before the operation.

## Archetypes

An archetype is an **unordered, duplicate-free set of nominal component types**. Its identity
is that set of type names - there are **no field names and no accessors**. A component's type
name *is* both the component and how you reach it (`h.mass`).

**Primitives:** `int`, `float`, `char`, the fixed-width ints, and `opaque`.

**Components** are either a *reference* to an existing type or an *inline definition*:

```arche
mass :: float;           // nominal type (lowercase = type)

Particle :: arche {
  mass,                  // reference an existing component type
  charge :: float        // inline definition (mints a `charge` component)
}
```

To carry two values of the same backing, **mint two distinct types** - you never alias one
type under two names:

```arche
health :: int;
shield :: int;
Unit :: arche { health, shield }   // two int columns, reached as h.health / h.shield
```

**Set identity - `{a, b}` == `{b, a}`.** Two archetypes with the same component set *are* the
same shape and share one pool; component order is irrelevant. A component type repeated in one
archetype is a compile error (it would be unreachable):

```
A :: arche { health, shield }
B :: arche { shield, health }   // same shape as A

[1000]A
[1000]B            // ERROR: shape already allocated (B is the same shape as A)
```

**Tuples** are named flat sugar: `pos (x, y) :: float` mints the flat component types
`pos_x` and `pos_y` of the shared type - the suffixes are part of the *name*. (One level
only - no nested tuples.) Reached as `h.pos_x` / `h.pos_y`. `pos (x, y) :: float` and
`vel (x, y) :: float` are **distinct** (`pos_x` != `vel_x`); reuse is by name.

```arche
pos (x, y) :: float;
Body :: arche { pos_x, pos_y }   // two flat columns
```

A shape's storage is one fixed-capacity static pool declared with `[N]Foo` (a declaration —
no trailing `;`, no `static` keyword: top-level position implies static storage).
There is no `alloc` and no resizing - all capacity is reserved upfront. Allocating the same
shape twice (under any name) is a compile error.

**Pool extents: `.count` and `.capacity`.** A pool has its own size vocabulary, distinct from a
sequence's `.length`:

- `Foo.count` — the number of **live rows** (entities currently in the pool). This is the
  iteration bound: `for (i := 0; i < Foo.count; i = i + 1) { … }`.
- `Foo.capacity` — the **allocated slots** `N`. Fixed storage size; not an iteration bound.

A pool is not a sequence, so `Foo.length` is rejected — use `.count` or `.capacity`. The same goes
for a bare column (`Foo.col.length` is rejected): a column becomes a `[]T` only by **slicing** it.

**A column as a buffer.** Slicing a single-field pool's column yields a `[]T` view over its storage —
this is how a pool backs a scratch/IO buffer without a large stack array ([W0026](explain/W0026.md)):

```arche,ignore
b :: char;
Buf :: arche { b }
[65536]Buf;                                       // a 64 KB byte buffer, statically allocated

io.read_chunk(fd, Buf.b[0:Buf.capacity], 65536)(chunk:);   // []char view over the pool's storage
```

The slice is a **borrow** of the pool's (stable, mutable) storage — pass it to borrowing readers/IO.
It has no ownership to transfer, so it is not `move`d (doing so is flagged — [W0027](explain/W0027.md)).

## Array-oriented operations

Operations on archetype columns apply across the entire collection without explicit loops:

```arche
pos_x :: float;
vel_x :: float;
Particle :: arche { pos_x, vel_x };

[1000]Particle(1000) { pos_x: 0.0, vel_x: 2.0 };
```
```arche
// updates each position by its velocity, across the whole column
Particle.pos_x = Particle.pos_x + Particle.vel_x;
fmt.assert(Particle.pos_x[0] == 2.0, "whole-column add\n")();
```

This iterates all elements, updating each position by its velocity. Inside a map the
component type names are available directly:

```arche
step :: map (query { pos_x, vel_x }) {
  pos_x = pos_x + vel_x;
}
```

## Indexing

Individual element access requires explicit column reference:

```
Particle.mass[i]      // scalar column
Particle.pos_x[i]     // tuple column (flat access), x component of position at i
messages.text[i][j]   // 2-D: chained, one index per bracket
```

Multi-dimensional access is **chained** — `a[i][j]`, one index per bracket (the former comma form
`a[i, j]` is gone). Chaining lets **each index carry its own failure policy**, which a single
comma-bracket could not express:

```
grid[row] !clamp [col] !abort   // clamp the row into range; abort if the column is out of bounds
```

A partial index yields the sub-row as a `[]T` slice (`messages.text[i]` is a `[]char`), so the chain
is just slice indexing applied one dimension at a time.

This keeps the language focused on whole-array transformations.

## Arrays and slices: `[N]T` values, `[]T` slices

Apart from archetype columns, Arche has two array forms — a sized value and a borrowed view.

**`[N]T` — a sized value.** `buf: [8]int` is stack storage of exactly `N` elements. It is a
**value**: it owns its storage, it is mutable (it's yours), and binding/assigning it transfers or
duplicates it under the ownership rules below. `.length` is the compile-time count `N` (arrays and
slices have a `.length`, never a `.cap`/`.capacity` — capacity is a pool concept; see *Pools*).
Indexing is bounds-checked; a provably in-range literal/loop index elides the check.

**`[]T` — a slice (fat pointer).** A slice is a `{ptr, len}` view whose length is carried at
**runtime**, so it appears in signatures without a size. A slice never owns storage — it borrows an
array's. It exists at the **function boundary** (a parameter, or a return), not as a free-floating
local you build up. Two modes, set by the ownership keyword:

```arche
sum  :: func(xs: []int) -> int {              // borrowed: READ-ONLY view, source stays alive
  total := 0;
  for (i := 0; i < xs.length; i = i + 1) {
    total = total + xs[i];
  }
  return total;
}
fill :: func(own xs: []int) -> []int {        // owned: mutable, movable, single writer
  for (i := 0; i < xs.length; i = i + 1) {
    xs[i] = i;
  }
  return move xs;
}
```

- A **borrowed** `xs: []T` is read-only — writing through it is a compile error. You can hand out
  as many borrows as you like precisely because none can mutate.
- An **owned** `own xs: []T` is the mutable, length-carrying buffer handle: the caller `move`s a
  buffer in, the callee mutates it and may hand it back. Obtaining a *mutable* slice consumes its
  source (it's a move), so there is never a second live writer — no mutable aliasing.

**Decay.** A sized `[N]T` **decays** to a `[]T` at a call: the compile-time count becomes the
slice's runtime length. Sizing flows one way — a `[]T` cannot satisfy a `[N]T` parameter (a slice's
length isn't statically known), so that is rejected.

```arche
a: [8]int;
for (i := 0; i < a.length; i = i + 1) { a[i] = 2; }
total := sum(a);        // a decays to []int (a borrow) — a stays alive, .length == 8 at runtime
fmt.assert(total == 16, "eight 2s sum to 16\n")();
view := fill(move a);   // move a in (a consumed), mutate, hand the fat pointer back, rebind
fmt.assert(sum(view) == 28, "0+1+...+7 == 28\n")();
```

**Sub-slicing.** `buf[lo:hi]` is a read-only borrowed sub-view — `{ptr+lo, hi-lo}`, no copy. `lo`/`hi`
are optional: `buf[:hi]`, `buf[lo:]`, `buf[:]`. It is bounds-checked (`0 <= lo <= hi <= length`,
aborts otherwise) and, being a borrow, consumes nothing — the base stays fully alive. Use it to hand
a sub-range to a reader (`sum(xs[2:5])`) or bind a view (`mid := xs[2:5]`).

```arche
a: [8]int;
for (i := 0; i < a.length; i = i + 1) { a[i] = 3; }
n := 5;
total := sum(a[2:5]);   // borrow elements 2..4 — a untouched
mid := a[:n];           // prefix view; mid.length == n
fmt.assert(mid.length == 5, "prefix length\n")();
fmt.assert(total == 9, "three 3s sum to 9\n")();
```

**Lifetime.** Storage lives in the stack frame that declared it and is reclaimed when that frame
returns; `move` transfers the *right* to a buffer, never the memory. The one rule the compiler
enforces: a function may not **return a slice that traces to its own local** `[N]T` (it would
dangle) — a returned slice must trace back to a buffer passed *in*.

There is no `for x in array`. Iterate a buffer with a C-style `for (i := 0; i < xs.length; i = i + 1)`,
and process archetype columns with a `map`.

## Effects: systems (`system`, `each`)

**Effects DO things; functions ARE things.** A `func` is *pure* — it can't call a `#foreign`
extern, run an `Eff`, or `insert`/`delete`. Anything that performs an effect is a **system**: an
action the schedule runs, never a value you call.

```
greet :: system { fmt.printf("hi\n"); }   // an action — scheduled, not called
```

A `system` takes **no parameters and returns nothing**. It is named in a `#run` schedule and the
runtime runs it; a program's **entry point is a `#run` schedule**, not a decl named `main` (a `main`
decl is a hard error, [E0225](explain/E0225.md)). To run an effect **per matching row**, use
`each (query { … })` — the effectful sibling of `map`: where a `map` body is a branch-free column
kernel, an `each` body may branch, call, `insert`/`delete`, and run effects, once per row. Being a
sequential per-element fan, it is a loop: `continue` skips the rest of the current element and advances to
the next row (guard-clause style). There is no `break` — stopping a fan early is incoherent.

`proc` is **reserved for the foreign boundary** — only a `#foreign`/`@syscall`/`@intrinsic`
declaration (or a `@drop` hook) is a `proc`. A non-foreign `proc` is an error
([W0030](explain/W0030.md)).

A system handles setup, orchestration, and whole-collection array ops:

```arche
pos_x :: float;
vel_x :: float;
Particle :: arche { pos_x, vel_x };

[1000]Particle(1000);

advance :: system {
  Particle.pos_x = Particle.pos_x + Particle.vel_x;   // array op over the whole column
}

#run advance
```

## Maps (`map`)

Maps perform **data-driven transformations** over all matching archetypes.

```arche
pos_x :: float;
vel_x :: float;
Particle :: arche { pos_x, vel_x };

[1000]Particle(1000) { pos_x: 0.0, vel_x: 1.5 };

step :: map (query { pos_x, vel_x }) {
  pos_x = pos_x + vel_x;
}
```

A **schedule** drives a map by naming it in `#run` — the runtime owns the loop (there is no
hand-written driver). The map needs a pool whose shape supplies its components:

```arche
#run step
```

- executes when its name appears in a `#run` schedule (a bare leaf, or inside `seq`/`par`)
- automatically matches any archetype in scope containing the required component types
- binds those components inside the map body
- operates on whole columns (array-first)

So a map over `pos` and `vel` applies to any archetype with those components (`Player`,
`Mob`, `Projectile`, …) without naming them explicitly.

A map body is **only** column transforms (`col = expr`). It is a loopless, branch-free kernel:
the runtime owns the iteration, so the same source vectorizes on the CPU today and runs on the
GPU later. Control flow (`if`/`for`/`while`/`match`), loops, local bindings, and calls are
rejected in a map body (E0046) — put those in a `func` or an `each`/`system`.

### Conditional behavior

Inside a kernel, conditionals are values, not branches. A comparison produces `0`/`1`, and
**`select(cond, a, b)`** is the branch-free ternary — `cond` nonzero picks `a`, else `b`:

```arche
vel :: float;
pos :: float;
Body :: arche { vel, pos };
[8]Body(8);

dampen :: map (query { vel, pos }) {
  // kill velocity below a threshold (mask), and clamp with a branch-free select
  vel = vel * (pos > 10);
  vel = select(pos > 100.0, 0.0, vel);
}
```

`select` lowers to LLVM `select`: it vectorizes and has no GPU warp divergence, so it is the
portable conditional everywhere a `map` runs. There is no `if` in a kernel by design — a
per-element branch would defeat vectorization / GPU dispatch.

## Functions (`func`)

A function **IS a value**: it computes results from its inputs, with no side effects. It is the
**one non-foreign callable** — pure logic, whatever its shape.

```arche
drag_factor :: func(x: float) -> float {
  return x * 0.98;
}
```

- **Results via `-> T` *or* an out-param list.** A `func` produces its result either as a single
  `-> T` return *or* as an out-parameter list `(out, …)` — the form `proc` used to carry. A pure
  computation that yields **several** values or fills a buffer is a `func(in)(out, …)`:

  ```arche
  divmod :: func(a: int, b: int)(q: int, r: int) {
    q = a / b;          // write the out-params; no `return`
    r = a - q * b;
  }

  entry :: system {
    divmod(17, 5)(q:, r:);          // call mirrors the signature: foo(in)(out)
    fmt.assert(q == 3 && r == 2, "17 / 5 = 3 rem 2\n")();
  }

  #run entry
  ```

- **Pure** - a func body may not perform effects (call a `#foreign` extern, run an `Eff`, or
  `insert`/`delete`); this is enforced as a hard error. Effects belong in a `system`/`each`/`map`.
- **Never foreign.** A foreign C function is a `proc` *inside* a `#foreign` block, never a `func`.
  There is no `unsafe` qualifier in the language.
- Usable inside expressions; a func call is a value, so it is freely usable as an **in**-argument,
  but an out-result fills a caller place (`name:`), never an in-position.

## `func` vs `map` vs `system`

| Kind              | Binding                                    | Is it a value? | Purpose                                |
| ----------------- | ------------------------------------------ | -------------- | -------------------------------------- |
| `func`            | `name :: func(in) -> T` or `func(in)(out)` | yes (`-> T`)   | pure computation; results via return **or** out-params |
| `system` / `each` | `name :: system { … }`                     | no             | effects; scheduled by `#run`           |
| `map`             | `name :: map (query {components})`         | no             | data transform over archetypes         |
| `proc`            | `name :: proc(in)(out)` *(in `#foreign`)*  | no             | foreign/`@syscall`/`@intrinsic` primitive **only** |

- `func`: "compute a value" - `r := area(w, h)`, or `divmod(17, 5)(q:, r:)` for multiple results
- `system`: "do this effect" - named in a schedule (`#run entry`)
- `map`: "run this on _any data shaped like this_" - `#run step`
- `proc`: a foreign primitive — declared only inside a `#foreign` block

## GPU maps (`@gpu`)

Because a `map` is already a loopless, branch-free, per-element kernel, it maps directly onto a GPU compute
shader. The dispatch decision lives at the **call site** — annotate the `run` with `@gpu` and the compiler
can *also* lower that kernel to a compute shader. The same map can run on the CPU from one driver and the
GPU from another:

```arche
pos :: float;
vel :: float;
Mover :: arche { pos, vel };
[256]Mover(256);

@gpu
integrate :: map (query { pos, vel }) {
  pos = pos + vel;
}

#run integrate
```

- `@gpu` is **additive and transparent**: it never changes the result — it only enables GPU execution.
  A program behaves identically with or without a GPU present.
- **`arche build --gpu`** produces a normal executable that actually runs each `@gpu` map on the GPU: the
  map's shader is compiled to SPIR-V and embedded in the binary, and scheduling an `@gpu` map dispatches it
  via an in-binary Vulkan runtime. If there is no GPU (no device, no driver, or the build had no shader compiler),
  the dispatch **falls back to the CPU map automatically** — so a `--gpu` binary is always correct. Because
  arche `float` is **f32 on both the CPU and the GPU**, the two paths are the same numeric machine: the GPU
  result matches the CPU bit-for-bit.
- Plain `arche build` (no `--gpu`) is a pure CPU binary with no Vulkan dependency — `@gpu` is inert there.
- `arche build --emit-gpu=<dir>` writes the GLSL compute shader (one storage buffer per column) for each
  `@gpu` map as a side artifact, without changing the executable.
- Gates: `make test-gpu` validates the shaders to SPIR-V, `make test-gpu-run` dispatches via a standalone
  runner, and `make test-gpu-exe` runs a real `--gpu` executable on the GPU and checks its output.
- The GPU-executable subset today is a single static pool whose columns are all float, with arithmetic and
  `select`; anything outside it runs on the CPU. Runtime residency/async and instanced rendering are staged
  — see `docs/OPEN_ITEMS.md`.

## Collectives (`reduce` / `scan` / `sort`)

A `map` is per-element; a **collective** is a whole-column operation. The three collectives are
compiler builtins, not keywords, and they run from a `system` over a pool's columns:

- **`reduce(op, Pool.col)`** folds a column to a scalar.
- **`scan(op, Pool.col)`** overwrites each element with the inclusive prefix fold (a running
  reduction), in place.
- **`sort(Pool.key)`** sorts the whole pool ascending by a key column, permuting every column
  together so each entity's fields stay aligned. Pass `desc` for descending order —
  `sort(Pool.key, desc)` (`asc` is the default).

`reduce`/`scan` take a **monoid** as their first argument: an associative operator plus an
identity. The operator is a fixed built-in set — `+` (identity 0), `*` (1), `min` (+∞), and
`max` (−∞) — not an arbitrary expression or function: it must have a compiler-known identity and
be associative (a bad op is rejected, E0048). Associativity is *trusted* (as in Futhark / C++) —
it is what lets the runtime fold in any order, and later in parallel on the GPU. A fold of an
empty pool returns the identity.

`reduce` is a **value** (use it in an expression — `total := reduce(+, N.col)`); `scan` and
`sort` are **actions** (statements that mutate the column in place). This is the same value/action
split as `func` vs `system`.

```arche
#import { fmt }
score :: int;
N :: arche { score };
[5]N(5);

tally :: system {
  N.score = { 5, 1, 4, 2, 3 };
  total := reduce(+, N.score); // 15
  best  := reduce(max, N.score); // 5
  scan(+, N.score); // N.score becomes the running sum: 5 6 10 12 15
  sort(N.score); // sorts the pool ascending by score
  fmt.printf("total=%d best=%d\n", total, best);
}

#run tally
```

A collective is rejected inside a `map` body (E0047): a kernel sees one element at a time, so a
whole-column reduction belongs in a `system`.

## Totality and failure policies (`!policy`)

Most arche operations are total by construction (proven indexing, ok-valued insert). The few that
can still fail at runtime — an out-of-bounds index, a slice past the end, a divide-by-zero — resolve
that failure **locally, at the site**, with a *failure policy* written `expr !policy`:

```
v := samples[k] !clamp;   // out-of-range index → clamped into [0, len)
grid[x] !clamp = c;       // the write form attaches the policy to the indexed lvalue
n := count / d !zero;     // divide-by-zero → 0
raw := buf[j] !undefined; // opt out of all runtime safety: raw access, no check (UB if OOB)
crash := buf[j] !abort;   // the only policy that terminates — a visible, deliberate crash site
```

`!undefined` is the raw, runtime-unsafe escape hatch, so it is **forbidden by default** — an ordinary
build rejects it ([E0126](explain/E0126.md)); pass `--allow-undefined` (implied by `--unchecked`) to
opt in. It is *never* allowed inside a `policy` body.

**A policy is a MACRO** — the compiler inlines its body at the op, with the op's operands bound as
mutable locals, then runs the *raw* op on whatever they now are. Nothing is built in: `abort`,
`clamp`, `wrap`, `undefined`, `zero` are all ordinary `policy` decls in `core`, and you write your own.
The only irreducible primitive is `_exit` (a libc extern); `abort` is just a policy that calls it.
`!undefined` is the *empty* policy — no mutation ⇒ the raw op.

Only `!abort` (or a user policy that calls `_exit`) *deliberately* terminates — `!undefined` can
still fault as UB, and a buggy policy can leave the raw op out of bounds. But the deliberate crash is
per-site rather than smeared over a whole system:

- A **`func`'s** baseline default is `clamp`. An unannotated fallible op in a func clamps — it can't
  crash unless you explicitly opt into `!undefined`/`!abort` or a crashing custom policy.
- **Effectful code** (a `system`/`each`/`map`) has baseline default `abort`. Either default is
  overridable per-decl with `@default(<policy>)` (e.g. `@default(clamp) hot :: system {…}`), or
  globally with `--unchecked` (→ `undefined`). The implicit default is surfaced as an editor inlay
  so it's never a surprise.

The bounds prover decides the rest: a **provably-safe** access needs no policy (an explicit one is
the dead-policy lint `W0018`); a **provably out-of-bounds** constant access is a compile error
(`E0097`) regardless of any policy — a statically-wrong index is a bug, not a runtime case. So a
policy attaches only to the ops whose safety the compiler can't decide.

Policies are ordinary `policy` decls, tagged by op category, living in a namespace separate from
funcs/procs (so a `zero` policy and a `zero` func coexist). `core` ships the common ones; you add more.
A bounds policy binds `(len, i)` and **mutates `i`** (a divide policy binds and mutates `a, b`):

```arche
@policy(bounds) clamp :: policy(len: int, i: int) {   // mutate i into range, then `base[i]` runs raw
  if (i < 0)    { i = 0; }
  if (i >= len) { i = len - 1; }
}
@policy(bounds) abort :: policy(len: int, i: int) {   // terminate on a bad index — `abort` is a policy
  if (i < 0 || i >= len) { _exit(134); }
}
@policy(divide) zero :: policy(a: int, b: int) { if (b == 0) { a = 0; b = 1; } }  // n/d → 0 when d==0
```

### Pool overflow & `ok`

A pool is the third policy category. `insert` resolves an overflow handler — per-call `?name` › the
pool's declared `[N]P ?name` › `@default(proc, pool, X)` › the **`reject`** baseline — and the choice
gates whether the call must handle `ok`:

- **`reject`** (the default) is *fallible*: a full pool drops the row and reports `ok = 0`. Because the
  insert can fail observably, the call **must handle `ok`** — bind it to a real name (not `_`). Discarding
  it (`insert(P{…})(_:, _:)`) ignores a real failure: the `discarded_ok` lint, **error by default**
  (`--discarded-ok=warn|allow` demotes it; `@allow(discarded_ok)` suppresses one site).
- **`?abort`** crashes loudly on overflow (a full pool is a programmer error, like an out-of-bounds index),
  and **`?evict_*`** custom policies make room. These are *infallible* — the caller never sees `ok = 0` —
  so there is no `ok` to handle: `insert(P{…})` with no out-list (or `(h:)` for just the handle) is the
  clean form. `delete(h)` is likewise infallible (it aborts on a stale handle).

So you never silently ignore overflow: either the pool declares a visible policy that handles it, or you
handle `ok` at the call site. A policy-less pool's implicit policy is surfaced as an editor inlay — a ghost
`?reject` (or the `@default(proc, pool, X)` override) on the declaration — so the failure behaviour is never
a surprise. A pool policy binds `(count, cap, ok, slot)` — set `ok = 0` to reject, or redirect `slot` to a
victim row to evict:

```arche
[8]Conn ?abort;              // a full connection pool crashes — overflow is a bug here
[64]Job ?evict_oldest;       // … or evict the oldest job to make room
[16]Pending;                 // no policy → reject: every `insert` must check `ok`
```

### Narrowing crash sources

`!abort` is the *deliberate* crash site, and these flags ban it — but no single flag proves a build
crash-free. A program can still fault through a custom `policy` that calls `_exit`, or one that fails
to bring its operands in range and leaves the raw op out of bounds. Policies are ordinary user code.
The raw `!undefined` opt-out is already off by default; the flags control the *built-in* abort:

- `--no-abort` — any op resolving to `!abort` (implicit **or** explicit) is a compile error: the
  binary contains no abort-policy site. (It says nothing about a `_exit`-calling custom policy.)
- `--no-implicit-abort` — only the *implicit/default* `!abort` errors, so every fallible op must be
  explicitly annotated (a deliberate, visible `!abort` is still allowed).
- `--allow-undefined` — opts the raw `!undefined` op back IN. It is **forbidden by default** (it reads
  out of bounds with no check — UB), so an ordinary build already rejects it; this flag is the
  trusted-build escape hatch. (`--unchecked` implies it.) `!undefined` is *never* allowed inside a
  `policy` body regardless of this flag — a policy must stay total ([E0213](explain/E0213.md)).

To get close to a crash-free build you add `--no-abort` (the raw op is already off) **and** audit the
policies you use (the bundled `clamp`/`zero`/`wrap` are total and don't call `_exit`; a policy you
write is your responsibility). These apply to your code; the bundled core/stdlib are exempt, and
`#foreign`/FFI procs are outside the map — a foreign C boundary is trusted, not policy-tracked.

### Errors as values: `insert` / `delete`

A fixed-capacity pool can fill up. Rather than abort, `insert`/`delete` report the recoverable
failure **as a value** through a mandatory `ok` out-param — so they are **statement-only**, never a
value or nested in an expression:

```arche
mass   :: float;
charge :: float;
Particle :: arche { mass, charge };

[8]Particle(0);           // capacity 8, starts empty
```
```arche
insert(Particle{ mass: 1.0, charge: 0.1 })(h:, ok:);   // h: the generation-checked handle, ok: 0 if the pool was full
if (!ok) { fmt.printf("pool full\n"); } // handle the full-pool case at the call site
fmt.assert(ok == 1, "insert succeeded\n")();
fmt.assert(Particle.mass[0] == 1.0, "inserted mass\n")();
delete(h)(ok:);                         // ok: 0 on generation exhaustion
fmt.assert(ok == 1, "delete succeeded\n")();
```

Use `_` to discard either out (`insert(P{ x: … })(_:, _:)`). The legacy value form (`h := insert(…)`,
`i32(insert(…))`) is gone (`E0096`). `insert` is **total** (overflow → `ok = 0`). `delete`'s default
is `!abort`: a *stale* handle (use-after-free) is a bug and aborts. `delete(h)(ok:)` reports
generation exhaustion (a resource limit) via `ok` instead.

## Enums and `match`

An `enum` is a **distinct, int-backed type** with named variants. Variants
auto-increment from 0, or take an explicit `= N`:

```arche
Method :: enum { get, post, delete }      // get = 0, post = 1, delete = 2
Status :: enum { ok = 200, not_found = 404 }
```

`match` dispatches on an enum, integer, or string. It is **exhaustive** for
enums/integers — list every variant or add a `_` catch-all, else it's a compile
error ([E0210](explain/E0210.md)). Each arm is `pattern : statement` (or a block):

```arche
handle :: func(m: Method) -> int {
  code := 0;
  match m {
    Method.get    : code = 1;
    Method.post   : code = 2;
    Method.delete : code = 3;
  }
  return code;
}
```

`match` desugars to a direct-dispatch if-chain — no jump-table indirection in the
source, no function pointers. String patterns compare with the pure `streq` helper:

```arche
route :: func(path: []char) -> int {
  code := 0;
  match path {
    "/"      : code = 1;   // home
    "/about" : code = 2;   // about
    _        : code = 0;   // not found
  }
  return code;
}

entry :: system {
  fmt.assert(handle(Method.post) == 2, "POST -> 2\n")();
  fmt.assert(route("/about") == 2, "/about -> 2\n")();
}

#run entry
```

This `enum` + `match` pairing is how Arche does compile-time, exhaustive dispatch —
the data-oriented alternative to runtime function-pointer tables (see
[E0211](explain/E0211.md)). Arche has **no runtime function values** at all: there
are no proc/func-typed parameters and no callback indirection — behavior is selected
by data (a `match` arm, a row routed into a pool, the next `system` in a schedule),
never by a stored pointer. The "callback" patterns this replaces are spelled out in
[docs/design/callbacks-as-data.md](design/callbacks-as-data.md).

## Ownership: borrow, `move`, `copy`

Functions are **pure by use** - a function never mutates its caller's data as a side effect.
The default parameter mode is a **read-only borrow**: a plain slice `xs: []T` (or any aggregate
borrowed by reference) is read-only (mutating it is a compile error) and is not consumed. Scalars
are passed by value. Arrays/slices and foreign `opaque` values are **move-only** (linear).

**`move` and `copy` are general value operators** — usable in any value position (a bind/assign
RHS, a `return`, a call argument), not only as call arguments:

- **`move x`** - transfer `x` (zero copy); `x` is consumed - it is **dead** afterward, with no
  revival. Using it again, including reusing the *same name* as an out-arg, is a compile error;
  bind a fresh name (`f(move x)(x:)`). `move` is the one-way hand-off.
- **`copy x`** - produce a *duplicate* (a memcpy); the source stays alive. (`copy` of an `opaque`
  is rejected - it is non-copyable; a runtime-length slice has no static size to clone.)

**A bare name is an implicit `move` — for move-only types.** Where ownership is *taken* — a bind
(`a := b`), an assign (`a = b`), a `return`, or an **`own`** parameter argument — a bare move-only
name (array/slice or opaque) transfers and is **consumed**. This is the default precisely because it
is the *cheap* operation: a bare hand-off never silently performs an expensive copy; you write
`copy` when you want the duplicate. Scalars and other `Copy` types are never consumed by a bare
name — they copy, as before. (The editor surfaces the elided transfer as a ghost **`move`** inlay,
so a consumed binding is always visible without the keyword.)

```
a := b;          // move: b is consumed (dead), a owns the storage
a := copy b;     // clone: b stays alive, a is independent
n := sink(buf);  // own param: buf moved in (consumed); a borrow param (xs: []T) would NOT consume
```

A bare name handed to a **borrow** parameter (`xs: []T`) is *not* consumed — it's a borrow, the
source stays alive. So borrow-vs-move is read straight off the callee's signature (`[]T` borrows,
`own []T` takes ownership); there is no separate borrow keyword at the call site.

To **fill a caller buffer in place**, a `#foreign` extern declares the buffer as an **in-out**
parameter (the same name in both lists) — the C-ABI shape. A non-foreign caller can't repeat that
proc shape, so it exposes the extern as a `func` returning an **`Eff`**: under-apply the extern,
writing `_` for the buffer in-slot, and let the *run site* allocate the buffer as the OUT. The
buffer is never an in-argument to copy or `move` — it is allocated caller-side at the `(buf: …)`
out-binding and filled in place:

```arche
#foreign {
  read :: proc(fd: int, buf: []char, len: int)(buf: []char, n: int);   // libc read(2)
}

read_into :: func(fd: int, len: int) -> Eff([]char, int) {
  return read(fd, _, len);   // `_` is the C-ABI in-slot; the buffer is the caller-allocated OUT
}

reader :: system {
  read_into(0, 8)(buf: [8]char, n:);            // `:` allocates the OUT buffer the extern fills
  fmt.printf("read %d bytes (first=%d)\n", n, buf[0]);
}

#run reader
```

- **You can't move out of a borrow.** `move`-ing a borrowed (non-`own`) array parameter - which
  includes an in-out buffer - is a compile error. You *may* `copy` a borrow.
- **A pool column (slice) is borrow-only.** A column view such as `Buf.b[0:Buf.capacity]` borrows
  the pool's shared, fixed storage — there is no ownership to transfer, so `move`-ing it does nothing
  and is flagged ([W0027](explain/W0027.md) `pointless_move`). Pass it as a plain borrow. (To get a
  consumable binding, bind the slice to a local first and `move` that local — the local is killed,
  the pool storage is not.)
- **Must-consume:** an opaque *local* must leave its scope before the end - moved into an
  `own` parameter, returned, or `insert`ed into a pool. Otherwise it is a compile error
  (`opaque value 'w' not consumed before scope end`). No implicit `drop`/RAII, no silent leak.

## Foreign resources: `opaque` types

There is no separate "extern type" map. A foreign resource (OS window, audio voice, file
pointer) is just a **nominal type aliased over `opaque`** - a pointer-width, C-owned cell that
Arche never reads, writes, or fabricates. Distinctness comes from the *name*, not a wrapper.

```arche
window :: opaque;
sound  :: opaque;

#foreign {
  window_open    :: proc(own title: []char, w: int, h: int)(w: window);   // out-only w = C return
  window_present :: proc(w: window, fb: []int, width: int, height: int)(fb: []int);  // fb in-out
  window_close   :: proc(own w: window)();
}
```

- An `opaque` value is passed to/from C **by value** - the cell *is* an `i64`, ABI-compatible
  with the native pointer (`HWND`, `FILE *`, …). No marshal, no slot table.
- `window` and `sound` are **distinct types** though both back to `opaque`: passing one where
  the other is expected is a compile error.
- `0` is the null cell; returning `NULL` from C yields `0`, and `if (w)` is a non-null check.
- A foreign resource can also live in a **pool**: put the type in an `arche` and use
  `[N]Foo` + generation-checked handles for capacity-bounded, use-after-free-safe storage.

```arche
render :: system {
  fb: [307200]int;                       // 640 * 480 framebuffer
  window_open("demo", 640, 480)(w:);
  window_present(w, fb, 640, 480)(fb);   // in-out: fb lent and handed back, stays live
  window_close(move w);              // `own` param consumes it - w dead afterward
}
```

## Multiple outputs and mutate-in-place

A `func` produces results via a single `-> T` return **or** an out-parameter list `(out)`. To
produce **several** results, or to mutate a value in place, give the `func` an out-param list -
the values are written into caller-provided places, never returned (this is the form `proc` used
to carry, now folded into the one pure callable):

```arche
sum_diff :: func(a: int, b: int)(s: int, d: int) {
  s = a + b;
  d = a - b;
}

sum_diff(10, 3)(s:, d:);   // s = 13, d = 7 - declared + scoped by the out-args
fmt.assert(s == 13 && d == 7, "sum and diff\n")();
```

**Filling a caller buffer (zero-copy).** A buffer that a callee writes is exposed as a `func`
returning an **`Eff`**: a `#foreign` extern declares the buffer as an in-out parameter (the C-ABI
shape), and the wrapper under-applies it with `_` in the buffer in-slot. The buffer is then the
**caller-allocated OUT** at the run site — no `own`, no `move`, no copy:

```arche
file :: opaque;

#foreign {
  arche_csv_read_chunk :: proc(fd: file, buf: []char, size: int)(buf: []char, n: int);
}

read_chunk :: func(fd: file, size: int) -> Eff([]char, int) {
  return arche_csv_read_chunk(fd, _, size);   // `_` is the C-ABI in-slot; the buffer is the OUT
}
```

For a `#foreign` proc, the in-list maps the C argument order, an in-out name is an in-place
pointer write, and an **out-only** name maps to the C **return value** - so the same
`(in)(out)` shape describes both a `func`'s out-params and a foreign C function.

Out-args bind **left-to-right** in the callable's out-param order. Each is a new place (`x:`,
declared + zero-inited) or an existing live variable (`x`).

## Implicit loop codegen

Whole-column operations like `Particle.pos = Particle.pos + Particle.vel` don't have explicit
loops in source - the compiler generates them. This implicit loop codegen is critical to
performance (see [performance.md](performance.md)):

- **Column base hoisting**: column pointers are computed once before the loop and cached, not recalculated per iteration.
- **Vectorization**: the loop structure supports vector loads/stores for float/double columns.
- **Bounds checking**: count metadata is loaded once; element-wise indexing is bounds-safe by design.
- **Static allocation awareness**: codegen avoids pointer indirection where possible.

For static allocation, hoisting eliminates redundant address calculations that LLVM's
conservative alias analysis (on large structs) cannot optimize away.

## Design priorities

Arche prioritizes **access speed and predictability** over memory footprint:

- **Fast memory access**: columnar layout keeps related data together, enabling cache-friendly sequential access and SIMD.
- **Predictable memory usage**: fixed allocations mean stable, foreseeable memory patterns (no GC pauses, no resizing surprises).
- **Willing to use more memory**: upfront, column-oriented storage uses more RAM than pointer-based designs, but the tradeoff is worth it for speed and predictability.

It deliberately avoids pointers/references (columnar layout instead), dynamic memory
(fixed sizes upfront), classes/inheritance (archetypes instead), implicit iteration (loops
made explicit via maps), and complex type maps (primitives and columns only).

## Worlds (planned)

A **World** is a planned feature that will act as a collection of archetypes and a scope for
maps (syntax sketch: `world Simulation()`), allowing parallel data-driven computations.
**Not yet implemented** - currently maps operate on all matching archetypes in scope.
