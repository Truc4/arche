# The Arche language

A reference for Arche's memory model, type system, declarations, and the
`proc` / `func` / `sys` split. For tooling (CLI, editor, build) see
[tooling.md](tooling.md); for benchmarks see [performance.md](performance.md);
for doc comments and doctests see [DOCTESTS.md](DOCTESTS.md).

## Contents

- [Memory model: static allocation only](#memory-model-static-allocation-only)
- [Types and declarations](#types-and-declarations)
- [Numeric model](#numeric-model)
- [Archetypes](#archetypes)
- [Array-oriented operations](#array-oriented-operations)
- [Indexing](#indexing)
- [Arrays and slices: `T[N]` values, `T[]` slices](#arrays-and-slices-tn-values-t-slices)
- [Procedures (`proc`)](#procedures-proc)
- [Systems (`sys`)](#systems-sys)
- [Functions (`func`)](#functions-func)
- [`proc` vs `sys` vs `func`](#proc-vs-sys-vs-func)
- [Enums and `match`](#enums-and-match)
- [Compile-time callbacks](#compile-time-callbacks)
- [Ownership: borrow, `move`, `copy`](#ownership-borrow-move-copy)
- [Foreign resources: `opaque` types](#foreign-resources-opaque-types)
- [Multiple outputs and mutate-in-place](#multiple-outputs-and-mutate-in-place)
- [Implicit loop codegen](#implicit-loop-codegen)
- [Design priorities](#design-priorities)
- [Worlds (planned)](#worlds-planned)

## Memory model: static allocation only

**Arche has NO dynamic or heap allocation.** All memory is allocated statically at
program startup. This is a core design principle, not a limitation.

- **Explicit allocation**: `Archetype[N]` declares a fixed-capacity pool for an archetype shape. There is no `static` keyword — every top-level declaration is static-lifetime, so position alone marks the storage.
- **Static storage**: All pools have program lifetime. They exist from program start to end. No deallocation during execution.
- **Fixed size**: Each pool is immutable in size. No resizing, no growing vectors. Capacity is permanent once set.
- **Implicit cleanup**: At program end, all storage is implicitly freed. Explicit deallocation is **not allowed** (and unnecessary).
- **Initial live count**: `Archetype[N](M)` allocates capacity `N` and starts with `M` live rows; deleted slots are tracked in a free-list and reused by later inserts, eliminating fragmentation.

A pool declaration can include field initialization to set the live instances' values at allocation time:

```arche
Counter[5](5) { val: 7, score: 2.5 }
```

This reserves capacity for 5 instances, starts all 5 live, and initializes the `val` and `score` columns. Uninitialized columns are zero-initialized.

**Encouraged pattern** - plan memory upfront, allocate what you need, use predictably:

```arche
Particle[10000](10000) { active: 0 }
Enemy[5000](5000)
Projectile[1000](1000)

main :: proc() {
  run initialize;
  run update_loop;
  // implicit cleanup at end
}
```

**Why no dynamic memory?** Predictable memory usage (known at program start), zero
allocation overhead in hot loops, a fixed budget that forces clear thinking about
capacity, cache-friendly columnar layout, and no use-after-free / dangling pointers /
fragmentation.

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
meters :: float     // nominal type alias (distinct from any other float type)
seconds :: float    // meters != seconds, though both back to float
MAX :: 100          // value const (literal RHS)
x := 5              // runtime variable
```

The short forms are **elisions of the one canonical form**, not separate rules — `x :: e` is
`x : ⟨inferred⟩ : e`, `x := e` is `x : ⟨inferred⟩ = e`, and `x : T` is `x : T = 0` (the value
elided defaults to zero). The value separator is the mutability axis: `:` binds an immutable
compile-time **definition**, `=` binds mutable **storage**. Implicit and explicit are treated
identically.

These forms work at **file scope** too: `count := 0`, `limit : int = MAX`, `buf : int[8]` are
mutable globals. A mutable global's *initial value* must be compile-time-constant (static storage
is initialized at link time — there is no startup code), but the variable is still reassignable;
a non-constant initializer is an error. Implicit `flag : int` is exactly `flag : int = 0`.

**Name visibility and order.** All top-level names — definitions *and* mutable-storage bindings —
are visible across the whole file regardless of order: a proc may read a global declared below it,
just as it may call a func defined later. (Locals, by contrast, are visible only from their point
of introduction onward.)

A type alias is **zero-cost** - it erases to its backing after checking. Operators resolve
on the backing (so `meters + meters -> meters`, and mixed-alias arithmetic over the same
backing is fine), but **nominal identity is enforced at substitution boundaries**
(parameters, assignment, return): you cannot pass `meters` where `seconds` is expected.
This is the entire basis of foreign-resource safety (`file :: opaque` != `socket :: opaque`).

## Numeric model

- Base primitives: `int`, `float`, `char` (no `bool` type)
- Comparisons produce numeric values (`0` or `1`); conditions treat `0` as false, non-zero as true

```arche
x = a < b   // x is 0 or 1
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
small := i32(offset);   // truncate i64 -> i32
wide: i64 = i64(small); // widen i32 -> i64
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
mass :: float            // nominal type (lowercase = type)

Particle :: arche {
  mass,                  // reference an existing component type
  charge :: float        // inline definition (mints a `charge` component)
}
```

To carry two values of the same backing, **mint two distinct types** - you never alias one
type under two names:

```arche
health :: int
shield :: int
Unit :: arche { health, shield }   // two int columns, reached as h.health / h.shield
```

**Set identity - `{a, b}` == `{b, a}`.** Two archetypes with the same component set *are* the
same shape and share one pool; component order is irrelevant. A component type repeated in one
archetype is a compile error (it would be unreachable):

```arche
A :: arche { health, shield }
B :: arche { shield, health }   // same shape as A

A[1000]
B[1000]            // ERROR: shape already allocated (B is the same shape as A)
```

**Tuples** are named flat sugar: `pos (x, y) :: float` mints the flat component types
`pos_x` and `pos_y` of the shared type - the suffixes are part of the *name*. (One level
only - no nested tuples.) Reached as `h.pos_x` / `h.pos_y`. `pos (x, y) :: float` and
`vel (x, y) :: float` are **distinct** (`pos_x` != `vel_x`); reuse is by name.

```arche
pos (x, y) :: float
Body :: arche { pos_x, pos_y }   // two flat columns
```

A shape's storage is one fixed-capacity static pool declared with `Foo[N]` (a declaration —
no trailing `;`, no `static` keyword: top-level position implies static storage).
There is no `alloc` and no resizing - all capacity is reserved upfront. Allocating the same
shape twice (under any name) is a compile error.

## Array-oriented operations

Operations on archetype columns apply across the entire collection without explicit loops:

```arche
Particle[1000]
// ... insert particles
Particle.pos_x = Particle.pos_x + Particle.vel_x;
```

This iterates all elements, updating each position by its velocity. Inside a system the
component type names are available directly:

```arche
step :: sys(pos_x, vel_x) {
  pos_x = pos_x + vel_x;
}
```

## Indexing

Individual element access requires explicit column reference:

```arche
Particle.mass[i]      // scalar column
Particle.pos_x[i]     // tuple column (flat access), x component of position at i
messages.text[i, j]   // 2D indexing
matrices.data[i, x, y] // 3D indexing
```

This keeps the language focused on whole-array transformations.

## Arrays and slices: `T[N]` values, `T[]` slices

Apart from archetype columns, Arche has two array forms — a sized value and a borrowed view.

**`T[N]` — a sized value.** `buf: int[8]` is stack storage of exactly `N` elements. It is a
**value**: it owns its storage, it is mutable (it's yours), and binding/assigning it transfers or
duplicates it under the ownership rules below. `.length` / `.cap` are the compile-time count `N`.
Indexing is bounds-checked; a provably in-range literal/loop index elides the check.

**`T[]` — a slice (fat pointer).** A slice is a `{ptr, len}` view whose length is carried at
**runtime**, so it appears in signatures without a size. A slice never owns storage — it borrows an
array's. It exists at the **function boundary** (a parameter, or a return), not as a free-floating
local you build up. Two modes, set by the ownership keyword:

```arche
sum  :: func(xs: int[]) -> int { ... }        // borrowed: READ-ONLY view, source stays alive
fill :: func(own xs: int[]) -> int[] { ... }  // owned: mutable, movable, single writer
```

- A **borrowed** `xs: T[]` is read-only — writing through it is a compile error. You can hand out
  as many borrows as you like precisely because none can mutate.
- An **owned** `own xs: T[]` is the mutable, length-carrying buffer handle: the caller `move`s a
  buffer in, the callee mutates it and may hand it back. Obtaining a *mutable* slice consumes its
  source (it's a move), so there is never a second live writer — no mutable aliasing.

**Decay.** A sized `T[N]` **decays** to a `T[]` at a call: the compile-time count becomes the
slice's runtime length. Sizing flows one way — a `T[]` cannot satisfy a `T[N]` parameter (a slice's
length isn't statically known), so that is rejected.

```arche
a: int[8];
total := sum(a);        // a decays to int[] (a borrow) — a stays alive, .length == 8 at runtime
a := fill(move a);      // move a in (a consumed), mutate, hand the fat pointer back, rebind
```

**Sub-slicing.** `buf[lo:hi]` is a read-only borrowed sub-view — `{ptr+lo, hi-lo}`, no copy. `lo`/`hi`
are optional: `buf[:hi]`, `buf[lo:]`, `buf[:]`. It is bounds-checked (`0 <= lo <= hi <= length`,
aborts otherwise) and, being a borrow, consumes nothing — the base stays fully alive. Use it to hand
a sub-range to a reader (`sum(xs[2:5])`) or bind a view (`mid := xs[2:5]`).

```arche
total := sum(a[2:5]);   // borrow elements 2..4 — a untouched
mid := a[:n];           // prefix view; mid.length == n
```

**Lifetime.** Storage lives in the stack frame that declared it and is reclaimed when that frame
returns; `move` transfers the *right* to a buffer, never the memory. The one rule the compiler
enforces: a function may not **return a slice that traces to its own local** `T[N]` (it would
dangle) — a returned slice must trace back to a buffer passed *in*.

There is no `for x in array`. Iterate a buffer with a C-style `for (i := 0; i < xs.length; i = i + 1)`,
and process archetype columns with a `sys`.

## Procedures (`proc`)

**Procedures DO things; functions ARE things.** This split is fundamental and shows up in the
*signature itself* - a `func` and a `proc` differ in shape (the name is on the binding LHS;
`proc`/`func` are RHS value forms):

```arche
area   :: func(w: int, h: int) -> int            // a value: one return, no side effects
divmod :: proc(a: int, b: int)(q: int, r: int)   // an action: inputs (in), outputs (out)
```

A procedure performs an **action**. It is **not a value** - it has no return type. Instead it
declares its results as **out-parameters** in a second parameter list. An out-param is a
caller-provided place the proc writes **in place**:

```arche
divmod :: proc(a: int, b: int)(q: int, r: int) {
  q = a / b;          // write the out-params; no `return`
  r = a - q * b;
}

main :: proc() {
  divmod(17, 5)(q:, r:);          // call mirrors the signature: foo(in)(out)
  printf("%d %d\n", q, r);        // q and r are declared by the out-args, scoped here
}
```

- **No return.** A `proc` is an action, never a value. `x := some_proc(...)` does not parse -
  a proc call is a *statement*, written `foo(in)(out)`.
- **Out-params are read-write places.** The caller supplies the slot (`name:` declares +
  zero-inits it; `name` reuses an existing one); the proc may read and write it; the value is
  the caller's afterward.
- **In-out (a name in *both* lists).** The out occurrence shadows the in-list borrow, making it
  a zero-copy *fill-in-place* slot: the body writes the buffer with no `own` and no `move`, and
  the binding is never killed. For an `extern proc` the in-list also fixes the C argument order,
  so an in-out name is a C argument written in place while an out-only name maps to the C
  **return value**. (A borrowed in-out may be written but not `move`d - it is still the caller's
  buffer.)
- The out-list is optional: `proc main()` (or any effect-only action) simply has no outputs.

Procedures are also used for setup, orchestration, and whole-collection array ops:

```arche
Particle[1000](1000)

main :: proc() {
  Particle.pos_x = Particle.pos_x + Particle.vel_x;   // array op over the whole column
}
```

## Systems (`sys`)

Systems perform **data-driven transformations** over all matching archetypes.

```arche
step :: sys(pos_x, vel_x) {
  pos_x = pos_x + vel_x;
}

update :: proc() {
  run step;
}
```

- executes via the `run system_name` statement
- automatically matches any archetype in scope containing the required component types
- binds those components inside the system body
- operates on whole columns (array-first)

So a system over `pos` and `vel` applies to any archetype with those components (`Player`,
`Mob`, `Projectile`, …) without naming them explicitly.

### Conditional behavior

Conditionals can be written as mathematical expressions (comparisons produce 0 or 1):

```arche
dampen :: sys(vel, pos) {
  // multiply velocity by 0 if below threshold
  vel = vel * (pos > 10);
}
```

Systems also support `if`/`for` for control flow. Branchless math like `vel = vel * (pos > 10)`
avoids branch mispredictions and vectorizes well **when the condition is data-dependent and
unpredictable**. For *predictable* conditions the branch predictor is effectively free, and a
real branch can be faster by skipping the masked-off work entirely. Neither is universally
better - it depends on the data, so measure before converting one to the other.

## Functions (`func`)

A function **IS a value**: it computes one result from its inputs, with no side effects.

```arche
drag_factor :: func(x: float) -> float {
  return x * 0.98;
}
```

- **Exactly one return** - `-> T` is mandatory; there is no multi-return (that's what a
  proc's out-list is for) and no `out` parameters.
- **Pure** - a func body may not perform effects (call a proc, an extern, or a mutating
  builtin); this is enforced as a hard error. Effects belong in a `proc`.
- **Never `extern`.** `extern` always pairs with `proc` - a foreign C function is an *action*
  (`extern proc`), never a `func`. There is no `unsafe` qualifier in the language.
- Usable inside expressions; freely callable as a proc's **in**-argument (a func call is a
  value), but never as an **out**-argument (a value is not a place).

## `proc` vs `sys` vs `func`

| Kind   | Binding                   | Is it a value? | Purpose                        |
| ------ | ------------------------- | -------------- | ------------------------------ |
| `func` | `name :: func(in) -> T`   | yes            | pure computation, one return   |
| `proc` | `name :: proc(in)(out)`   | no             | an action; writes out-params   |
| `sys`  | `name :: sys(components)` | no             | data transform over archetypes |

- `func`: "compute a value" - `r := area(w, h)`
- `proc`: "do this, writing the results into these places" - `divmod(17, 5)(q:, r:)`
- `sys`: "run this on _any data shaped like this_" - `run step;`

## Totality and panics (`proc!`)

A plain `proc` and every `func` are **total**: a call cannot panic (abort). The one way to opt into
panic-capability is the **`proc!`** form:

```arche
risky :: proc!(…)(…) { … }   // may panic (hit an unhandled abort site)
safe  :: proc (…)(…) { … }   // total — proven not to panic
```

Panic-capability is **contagious and statically enforced**: a plain `proc`/`func` that calls a
`proc!` is a compile error (`E0095`) — mark it `proc!` too, or remove the panicking call. `func` is
always total, so there is no `func!`. (`extern`/FFI procs are outside the system — a foreign C
boundary is trusted, not panic-tracked.)

### Errors as values: `insert` / `delete`

A fixed-capacity pool can fill up. Rather than abort, `insert`/`delete` report the recoverable
failure **as a value** through a mandatory `ok` out-param — so they are **statement-only**, never a
value or nested in an expression:

```arche
insert(Particle, 1.0, 0.1)(h:, ok:);   // h: the generation-checked handle, ok: 0 if the pool was full
if (!ok) { … }                          // handle the full-pool case at the call site
delete(h)(ok:);                          // ok: 0 on generation exhaustion
```

Use `_` to discard either out (`insert(P, …)(_:, _:)`). The legacy value form (`h := insert(…)`,
`i32(insert(…))`) is gone (`E0096`). `insert` is **total** (overflow → `ok = 0`). `delete` is
inherently **`proc!`**: a *stale* handle (use-after-free) is a bug and still aborts — only generation
exhaustion (a resource limit) is reported via `ok` — so any `delete` makes its caller `proc!`.

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
handle :: proc(m: Method)() {
  match m {
    get    : printf("GET\n");
    post   : printf("POST\n");
    delete : printf("DELETE\n");
  }
}
```

`match` desugars to a direct-dispatch if-chain — no jump-table indirection in the
source, no function pointers. String patterns compare with the pure `streq` helper:

```arche
route :: proc(path: char[])() {
  match path {
    "/"      : home()();
    "/about" : about()();
    _        : not_found()();
  }
}
```

This `enum` + `match` pairing is how Arche does compile-time, exhaustive dispatch —
the data-oriented alternative to runtime function-pointer tables (see
[E0211](explain/E0211.md)).

## Compile-time callbacks

A proc/func-typed **parameter** is a callback: the caller passes a known proc/func
by name, and the callee is **monomorphized** per call site so the call lowers to a
**direct call** — there are no runtime function pointers (Arche has no runtime proc
values). The proc/func type may be inline or a named alias:

```arche
done_handler :: proc()()                         // a proc TYPE (callable, structural)

run_task :: proc(work: int, on_done: done_handler)() {
  // ... do the work ...
  on_done()();                                   // direct call to the bound proc
}

finish  :: proc()() { printf("done\n"); }
cleanup :: proc()() { printf("cleaned\n"); }

main :: proc() {
  run_task(5, finish)();     // compiler specializes run_task for on_done = finish
  run_task(9, cleanup)();    // …and a second specialization for on_done = cleanup
}
```

The callback argument is erased from the generated ABI; the chosen behavior is
encoded in *which* specialization is called. This covers "do X, then do Y when
done" where Y is fixed at the call site (it can also be forwarded to another
callback-taking proc). A proc/func type can be a parameter or a named binding, but
**not** an archetype component ([E0211](explain/E0211.md)) — archetypes are data.

## Ownership: borrow, `move`, `copy`

Functions are **pure by use** - a function never mutates its caller's data as a side effect.
The default parameter mode is a **read-only borrow**: a plain slice `xs: T[]` (or any aggregate
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

```arche
a := b;          // move: b is consumed (dead), a owns the storage
a := copy b;     // clone: b stays alive, a is independent
n := sink(buf);  // own param: buf moved in (consumed); a borrow param (xs: T[]) would NOT consume
```

A bare name handed to a **borrow** parameter (`xs: T[]`) is *not* consumed — it's a borrow, the
source stays alive. So borrow-vs-move is read straight off the callee's signature (`T[]` borrows,
`own T[]` takes ownership); there is no separate borrow keyword at the call site.

To **fill a caller buffer in place** you don't need `own` or `move` at all - use an **in-out**
parameter (the same name in both the in-list and the out-list). The out occurrence shadows the
in-list borrow, so the body writes the buffer in place and the binding is never killed:

```arche
read_into :: proc(fd: int, buf: char[], len: int)(buf: char[], n: int) {
  read(fd, buf, len)(buf, n);             // the extern fills the in-out buffer in place
}

buf: char[256];
read_into(0, buf, 256)(buf, n:);          // zero copy: buf lent in, handed back, stays live
```

- **You can't move out of a borrow.** `move`-ing a borrowed (non-`own`) array parameter - which
  includes an in-out buffer - is a compile error. You *may* `copy` a borrow.
- **Must-consume:** an opaque *local* must leave its scope before the end - moved into an
  `own` parameter, returned, or `insert`ed into a pool. Otherwise it is a compile error
  (`opaque value 'w' not consumed before scope end`). No implicit `drop`/RAII, no silent leak.

## Foreign resources: `opaque` types

There is no separate "extern type" system. A foreign resource (OS window, audio voice, file
pointer) is just a **nominal type aliased over `opaque`** - a pointer-width, C-owned cell that
Arche never reads, writes, or fabricates. Distinctness comes from the *name*, not a wrapper.

```arche
window :: opaque
sound  :: opaque

window_open    :: extern proc(own title: char[], w: int, h: int)(w: window)   // out-only w = C return
window_present :: extern proc(w: window, fb: int[], width: int, height: int)(fb: int[])  // fb in-out
window_close   :: extern proc(own w: window)()
```

- An `opaque` value is passed to/from C **by value** - the cell *is* an `i64`, ABI-compatible
  with the native pointer (`HWND`, `FILE *`, …). No marshal, no slot table.
- `window` and `sound` are **distinct types** though both back to `opaque`: passing one where
  the other is expected is a compile error.
- `0` is the null cell; returning `NULL` from C yields `0`, and `if (w)` is a non-null check.
- A foreign resource can also live in a **pool**: put the type in an `arche` and use
  `Foo[N]` + generation-checked handles for capacity-bounded, use-after-free-safe storage.

```arche
render :: proc() {
  w := window_open("demo", 640, 480);
  window_present(w, fb, 640, 480)(fb);   // in-out: fb lent and handed back, stays live
  window_close(move w);              // `own` param consumes it - w dead afterward
}
```

## Multiple outputs and mutate-in-place

A `func` returns exactly one value. To produce **several** results, or to mutate a value in
place, use a `proc` and its out-parameter list `(out)` - the values are written into
caller-provided places, never returned:

```arche
sum_diff :: proc(a: int, b: int)(s: int, d: int) {
  s = a + b;
  d = a - b;
}

sum_diff(10, 3)(s:, d:);   // s = 13, d = 7 - declared + scoped by the out-args
```

**Filling a caller buffer (zero-copy in-out).** A name in *both* the in-list and the out-list
is an **in-out** parameter: the caller lends a buffer (no `own`, no `move`, no copy), the proc
fills it in place, and the same live binding is handed back through the out-arg:

```arche
read_chunk :: proc(fd: file, buf: char[], size: int)(buf: char[], n: int) {
  arche_csv_read_chunk(fd, buf, size)(buf, n);   // the extern fills the in-out buffer in place
}

buf: char[65536];
read_chunk(fd, buf, 65536)(buf, n:);             // buf comes back filled; n is the byte count
```

For an `extern proc`, the in-list maps the C argument order, an in-out name is an in-place
pointer write, and an **out-only** name maps to the C **return value** - so the same
`(in)(out)` shape describes both Arche procs and foreign C functions.

Out-args bind **left-to-right** in the proc's out-param order. Each is a new place (`x:`,
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
made explicit via systems), and complex type systems (primitives and columns only).

## Worlds (planned)

A **World** is a planned feature that will act as a collection of archetypes and a scope for
systems (syntax sketch: `world Simulation()`), allowing parallel data-driven computations.
**Not yet implemented** - currently systems operate on all matching archetypes in scope.
