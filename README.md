# Arche

**Arche** is a small, experimental programming language built around a single idea:

> Programs should operate on _collections of structured data_ as a whole, not one element at a time.

It is intentionally **minimal**, **opinionated**, and **not designed for production use**.
This project exists primarily for exploration.

It's basically just my silly DOD and ECS concepts exploration. I have no idea what I'm doing.

## Philosophy

Arche is a **high-level language** with several strong design constraints:

- **Array-first**: Operations apply across entire collections by default.
- **No implicit row access**: You don’t work on individual elements unless you explicitly index.
- **Columns only**: All archetype data is columnar (arrays). No metadata. Columns are primitives or tuple columns.
- **Horizontal only**: No nested complex types, **no pointers**. Data layout is flat and explicit.
- **Tuple columns**: Multi-component fields like position vectors are stored as separate side-by-side arrays and accessed flat (`pos_x[i]`, `pos_y[i]`).
- **Pooled allocation with free-lists**: `static pool<Archetype>(N, N)` pre-allocates capacity for N entries. Deleted slots are tracked in a free-list and reused by subsequent inserts, eliminating fragmentation.
- **Minimal type system**: Primitives only: `int`, `float`, `char`. No booleans, no objects. This is a design principle, not a limitation.
- **Explicit structure over flexibility**: Users have complete control and visibility over data layout in memory.

This leads to a style that feels closer to **data pipelines** or **vectorized computation** than traditional imperative code.

## Memory Model: Static Allocation Only

**Arche has NO dynamic or heap allocation.** All memory is allocated statically at program startup. This is a core design principle, not a limitation.

### Allocation Strategy

- **Explicit allocation**: `static pool<Archetype>(N)` declares a fixed-capacity pool for an archetype shape.
- **Static storage**: All pools have program lifetime. They exist from program start to end. No deallocation during execution.
- **Fixed size**: Each pool is immutable in size. No resizing, no growing vectors. Capacity is permanent once set.
- **Implicit cleanup**: At program end, all storage is implicitly freed. Explicit deallocation is **not allowed** (and unnecessary).
- **Scope-implicit**: Scope (static vs world-scoped, when worlds exist) is determined by where the pool declaration appears.

### Allocation Initialization

A pool declaration can include field initialization to set all instances’ values at allocation time:

```arche
static pool<Counter>(5, 5) { val: 7, score: 2.5 };
```

This reserves capacity for 5 instances and initializes the `val` and `score` columns. Uninitialized columns are zero-initialized.

### Encouraged Pattern

Plan memory usage ahead of time, allocate what you need upfront, use predictably:

```arche
// Reserve every shape's pool with fixed capacity upfront
static pool<Particle>(10000, 10000) { active: 0 };
static pool<Enemy>(5000, 5000);
static pool<Projectile>(1000, 1000);

proc main() {
  // Run program with fixed memory budget
  run initialize;
  run update_loop;
  // Implicit cleanup at end
}
```

### Why No Dynamic Memory?

- **Predictability**: Memory usage is entirely known at program start.
- **Performance**: Zero allocation overhead; no malloc/free in hot loops.
- **Simplicity**: Fixed memory budget enforces clear thinking about capacity.
- **Cache-friendly**: Columnar layout with known sizes enables prefetching and SIMD.
- **Safety**: No use-after-free, no dangling pointers, no memory fragmentation.

## Worlds (Planned, Not Yet Implemented)

A **World** is a planned feature that will act as a collection of archetypes and a scope for systems. The syntax would be:

```arche
world Simulation()
```

Multiple worlds will allow parallel data-driven computations, with systems operating on all matching archetypes within a specific world. **This feature is not yet implemented.** Currently, systems operate on all matching archetypes in the scope.

## Types and declarations (`::` / `:=`)

Types are **nominal** — identity is the name, not the structure. A `::` declaration mints a
compile-time constant: a *type* if the right-hand side is a type, a *value const* if it is a
literal (the kind is inferred — there is no `type` keyword). Runtime variables bind with `:=`
(or `: T =`); `::` and `:=` never collide.

```arche
meters :: float     // nominal type alias (distinct from any other float type)
seconds :: float    // meters ≠ seconds, though both back to float
MAX :: 100          // value const (literal RHS)
x := 5          // runtime variable
```

A type alias is **zero-cost** — it erases to its backing after checking. Operators resolve on
the backing (so `meters + meters → meters`, and mixed-alias arithmetic over the same backing is
fine), but **nominal identity is enforced at substitution boundaries** (parameters, assignment,
return): you cannot pass `meters` where `seconds` is expected. This is the entire basis of
foreign-resource safety (`file :: opaque` ≠ `socket :: opaque`).

## Archetypes (`arche`)

An archetype is an **unordered, duplicate-free set of nominal component types**. Its identity
is that set of type names — there are **no field names and no accessors**. A component's type
name *is* both the component and how you reach it (`h.mass`).

**Primitives:** `int`, `float`, `char`, the fixed-width ints, and `opaque`.

**Components** are either a *reference* to an existing type or an *inline definition*:

```arche
mass :: float            // nominal type (lowercase = type)

arche Particle {
  mass,                  // reference an existing component type
  charge :: float        // inline definition (mints a `charge` component)
}
```

To carry two values of the same backing, **mint two distinct types** — you never alias one
type under two names:

```arche
health :: int
shield :: int
arche Unit { health, shield }   // two int columns, reached as h.health / h.shield
```

**Set identity — `{a, b}` == `{b, a}`.** Two archetypes with the same component set *are* the
same shape and share one pool; component order is irrelevant. A component type repeated in one
archetype is a compile error (it would be unreachable). Because collapse requires reusing the
same type *name*, it is always intentional:

```arche
arche A { health, shield }
arche B { shield, health }   // same shape as A

static pool<A>(1000);
static pool<B>(1000);        // ERROR: shape already allocated (B is the same shape as A)
```

**Tuples** are named flat sugar: `pos (x, y) :: float` mints the flat component types
`pos_x` and `pos_y` of the shared type — the suffixes are part of the *name*, the type comes
after `::`. (One level only — no nested tuples; one shared type — heterogeneous fields are just
separate components.) Reached as `h.pos_x` / `h.pos_y`. `pos (x, y) :: float` and
`vel (x, y) :: float` are **distinct** (`pos_x` ≠ `vel_x`); reuse is by name.

```arche
pos (x, y) :: float
arche Body { pos_x, pos_y }   // two flat columns
```

**Static allocation.** A shape's storage is one fixed-capacity static pool declared with
`static pool<Foo>(N)`. There is no `alloc` and no resizing — all capacity is reserved upfront.
Allocating the same shape twice (under any name) is a compile error.

## Array-Oriented Operations

Operations on archetype columns apply across the entire collection without explicit loops:

```arche
static pool<Particle>(1000);
// ... insert particles
Particle.pos_x = Particle.pos_x + Particle.vel_x;
```

This iterates all 1000 elements, updating each position by its velocity.

Inside a system function, the component type names are available directly:

```arche
sys step(pos_x, vel_x) {
  pos_x = pos_x + vel_x;
}
```

Same effect: iterate all elements element-wise.

## Indexing

Individual element access requires explicit column reference:

**Scalar columns:**

```arche
Particle.mass[i]
```

**Tuple columns (flat access):**

```arche
Particle.pos_x[i]   // x component of position at index i
Particle.pos_y[i]   // y component of position at index i
Particle.vel_vx[i]  // x component of velocity at index i
```

**Multi-dimensional arrays:**

```arche
messages.text[i, j]  // 2D indexing
matrices.data[i, x, y]  // 3D indexing
```

This keeps the language focused on whole-array transformations. Most operations work on entire columns, not individual elements.

## Numeric Model

- Base primitives: `int`, `float`, `char` (no `bool` type)
- Comparisons produce numeric values (`0` or `1`)
- Conditions treat `0` as false, non-zero as true

```arche
x = a < b   // x is 0 or 1
```

### Fixed-width integers

Always available alongside `int`: `i8`/`u8`, `i16`/`u16`, `i32`/`u32`, `i64`/`u64`, `i128`/`u128`, and `byte` (= `u8`). `int` is an alias for `i32`; `char` is `i8`. These map to native LLVM integers — no software emulation. Signedness is part of the type and selects the machine operation (`sdiv`/`udiv`, signed/unsigned compares, `sext`/`zext`).

```arche
offset: i64 = 3000000000;   // addresses past the 32-bit signed limit
offset = offset + offset;       // i64 arithmetic
count: u32 = 4000000000;    // unsigned 32-bit
b: byte = 255;
```

Convert between widths with a call-style cast (`sext`/`zext` to widen, `trunc` to narrow):

```arche
small := i32(offset);   // truncate i64 -> i32
wide: i64 = i64(small); // widen i32 -> i64
```

Integer literals adopt the type of their context (`x: i64 = 3000000000` types the literal as `i64`, avoiding 32-bit overflow). Operands of differing widths are widened to the larger width before the operation.

## Procedures (`proc`)

Procedures perform **explicit operations**.

```arche
static pool<Particle>(1000, 1000);

proc main() {
  Particle.pos_x = Particle.pos_x + Particle.vel_x;
}
```

- run once
- operate on explicitly referenced data (archetype names are handles to allocated shapes)
- array ops apply to the whole collection
- used for setup, orchestration, or control flow

## Systems (`sys`)

Systems perform **data-driven transformations** over all matching archetypes.

```arche
sys step(pos_x, vel_x) {
  pos_x = pos_x + vel_x;
}

proc update() {
  run step;
}
```

### Semantics

- executes via `run system_name` statement
- automatically matches any archetype in scope containing the required component types
- binds those components inside the system body
- operates on whole columns (array-first)

This means the system applies to any archetype with `pos` and `vel`, such as:

- `Player`
- `Mob`
- `Projectile`

without needing to reference them explicitly.

### Execution

Systems are invoked explicitly within procedures:

```arche
proc main() {
  run move;
  run dampen;
}
```

### Conditional Behavior

Conditionals work through mathematical expressions (comparisons produce 0 or 1):

```arche
sys dampen(vel, pos) {
  // multiply velocity by 0 if below threshold
  vel = vel * (pos > 10);
}
```

Systems support if/for statements for control flow. For maximum cache efficiency, prefer mathematical conditionals (as shown above) over branching when possible.

## Example

```arche
// Define tuple groups (each mints flat components, e.g. pos_x, pos_y)
pos (x, y) :: float
vel (vx, vy) :: float

// Reference the groups by name — they expand to their flat columns
// (`{ pos, vel }` == `{ pos_x, pos_y, vel_vx, vel_vy }`)
arche Particle { pos, vel }

// Reserve the pool at top level
static pool<Particle>(10000, 10000);

// Systems operate on matching archetypes, binding components by name
sys step(pos_x, vel_vx) {
  pos_x = pos_x + vel_vx;
}

sys dampen(vel_vx) {
  vel_vx = vel_vx * 0.99;
}

// Run systems
proc main() {
  run step;
  run dampen;
}
```

## Functions (`func`)

Functions are **pure computations** and do **not** mutate archetype data.

```arche
func drag_factor(x: float) -> float {
  x * 0.98
}
```

### Design rules for `func`

- cannot assign to arche fields
- cannot perform data transforms
- return a value
- used inside expressions

## `proc` vs `sys` vs `func`

| Kind   | Purpose          |
| ------ | ---------------- |
| `proc` | explicit logic   |
| `sys`  | data transforms  |
| `func` | pure computation |

- `proc`: “run this on _that data_”
- `sys`: “run this on _any data shaped like this_”
- `func`: “compute a value without modifying data”

## What Arche Is _Not_

- Not a general-purpose language
- Not optimized for production performance
- Not stable
- Not feature-complete

It deliberately avoids:

- **pointers and references** (use columnar layout instead)
- **dynamic memory** (allocate fixed sizes upfront)
- **classes / inheritance** (use archetypes instead)
- **implicit iteration** (make loops explicit via systems)
- **complex type systems** (primitives and columns only)
- **hidden behavior** (everything visible in the code)

## Design Vision

Arche is a **high-level language that eliminates complexity without sacrificing control**:

- **High-level abstractions** (archetypes, systems, array ops) without low-level complexity (no pointers, no manual indirection)
- **Explicit data layout** (you know exactly how data is arranged in memory)
- **Static memory model** (allocate once, use predictably, deallocate automatically)
- **No hidden costs** (no surprise allocations, no reference chasing, no garbage collection pauses)

Instead of complex type systems and dynamic features, language users make conscious decisions about data organization:

- **Strings**: No string type. Users implement fixed-size `char[]` arrays, packed arrays with length metadata, or handles to archetypes holding string data.
- **Collections**: Multi-column designs (`pos_x`, `pos_y`) sit side-by-side, enabling cache-friendly iteration.
- **References**: No pointers. Data is organized as columns within archetypes, not as complex nested structures.
- **Memory**: No dynamic allocation. All memory is allocated upfront with fixed capacity.

## Design Priorities

Arche prioritizes **access speed and predictability** over memory footprint:

- **Fast memory access**: Columnar layout keeps related data together, enabling cache-friendly sequential access and SIMD operations.
- **Predictable memory usage**: Fixed allocations mean stable, foreseeable memory patterns (no garbage collection pauses, no dynamic resizing surprises).
- **Willing to use more memory**: Upfront allocation and column-oriented storage use more RAM than pointer-based designs, but the tradeoff is worth it for speed and predictability.

This is a deliberate choice: **prefer fast, predictable memory access over minimizing memory footprint.**

## Design Analysis

The `design_analysis/` directory contains exploration and documentation of data layout patterns. Data drives the language design, and patterns and constraints discovered here shape feature decisions.

## Performance: C speed, scripting brevity

Single-threaded **transform** benchmark — a 10k-row CSV loaded once (off the clock), then the
transform body timed over 216,000 iterations. All engines produce identical checksums.

| Task | C (`-O3`) | **Arche** | pandas | Arche vs C | Arche vs pandas |
|------|----------:|----------:|-------:|:----------:|:---------------:|
| `revenue = price × quantity` | 1.24µs | **1.17µs** | 41.7µs | 0.95× (faster) | 36× faster |
| count `quantity > 0`         | 0.44µs | **0.52µs** | 28.6µs | 1.2×           | 55× faster |
| bucket timestamps            | 2.30µs | **2.35µs** | 29.2µs | 1.02×          | 12× faster |
| aggregate by region          | 6.22µs | **6.67µs** | 46.3µs | 1.07×          | 7× faster  |
| combined pipeline            | 6.36µs | **8.34µs** | 181µs  | 1.31×          | 22× faster |

**Verbosity** — lines to express a task end-to-end (load + compute + output):

| C | Arche | pandas |
|----:|------:|-------:|
| ~100 | ~38 | ~22 |

Arche runs within **~1–1.3× of hand-written `-O3` C** while the code reads like a vectorized
script: the transform is a single line in both Arche and pandas
(`Transaction.revenue = Transaction.price * Transaction.quantity`), where C needs an explicit
element loop plus ~80 lines of manual CSV parsing. Net — roughly C's speed, ~7–55× faster than
single-threaded pandas, in ~⅓ the code of C.

> Single-threaded, AVX2. polars/duckdb are multi-threaded and win on *large* data but lose on this
> tight per-op loop. Full cross-engine numbers + a 100M-row end-to-end run: `design_analysis/README.md`.

## Implicit Loop Implementation

Whole-column operations like `Particle.pos = Particle.pos + Particle.vel` don't have explicit loops in source code—the compiler generates them. This implicit loop codegen is critical to performance:

- **Column base hoisting**: Column pointers are computed once before the loop and cached, not recalculated per iteration
- **Vectorization**: Loop structure supports vector loads/stores for float/double columns
- **Bounds checking**: Count metadata is loaded once; element-wise indexing is bounds-safe by design
- **Static allocation awareness**: Codegen treats static and dynamic allocations differently to avoid pointer indirection where possible

For static allocation, hoisting eliminates redundant address calculations that LLVM's conservative alias analysis (on large structs) cannot optimize away.

## Why This Exists

Arche is an experiment in:

- **data-oriented design**
- **language minimalism**
- **making constraints visible in syntax**
- exploring how far you can go with just:
  - arrays
  - primitives
  - structured grouping

## Multi-return signatures

A function returns more than one value with a parenthesized return type `-> (T1, …, Tn)`. This is
a genuine multi-value return — the values are returned as an aggregate and destructured by a
multi-bind. There is no `out` parameter (gone) and no buffer-fill trickery:

```arche
func sum_diff(a: int, b: int) -> (int, int) {
  return a + b, a - b;
}

s, d := sum_diff(10, 3);   // s = 13, d = 7
```

A single return is just `count == 1` — there is no special case in the grammar or the AST; the
return type and the `return` statement are uniform lists. `return e1, …, en` lists the values in
signature order.

**Filling a caller buffer** uses the `own` ownership shape (see *Ownership* below): the function
takes the buffer `own`, fills it, and returns it; the caller `move`s it in and rebinds it (the
buffer threads through with no copy):

```arche
func read_chunk(fd: file, own buf: char[], size: int) -> (char[], int) {
  n := arche_csv_read_chunk(fd, buf, size);   // the extern fills the owned buffer
  return buf, n;
}

buf: char[65536];
buf, n := read_chunk(fd, move buf, 65536);   // buf comes back filled; n is the byte count
```

## Foreign resources: nominal `opaque` types

There is no separate "extern type" system. A foreign resource (OS window, audio voice, file
pointer) is just a **nominal type aliased over `opaque`** — a pointer-width, C-owned cell that
Arche never reads, writes, or fabricates. Distinctness comes from the *name*, not a wrapper.

```arche
window :: opaque
sound  :: opaque

extern func window_open(title: char[], w: int, h: int) -> window;
extern proc window_present(w: window, fb: int[], width: int, height: int);
extern proc window_close(own w: window);
```

- An `opaque` value is passed to/from C **by value** — the cell *is* an `i64`, ABI-compatible
  with the native pointer (`HWND`, `FILE *`, …). No marshal, no slot table; C authors write
  plain C with native pointer types.
- `window` and `sound` are **distinct types** though both back to `opaque`: passing a `window`
  where a `sound` is expected is a compile error. Identity is checked at boundaries (params,
  assignment, return).
- `0` is the null cell; returning `NULL` from C yields `0`, and `if (w)` is a non-null check.
- A foreign resource can also live in a **pool**: put the type in an `arche` and use
  `pool<Foo>` + generation-checked handles for capacity-bounded, use-after-free-safe storage.

### Ownership: read-only borrow by default; `own` + caller `move`/`copy`

Functions are **pure by use** — a function never mutates its caller's data as a side effect. The
default parameter mode is a **read-only borrow**: a plain `buf: T` is passed by reference (zero
copy), is read-only (mutating it is a compile error), and is not consumed (the caller keeps it
unchanged). Scalars are passed by value. A foreign `opaque` value is move-only (linear).

To mutate or consume an argument, the function declares the parameter **`own`** — it takes
ownership. The caller then chooses, **explicitly**, how to satisfy that — there is no implicit
copy and no implicit move:

- **`move x`** — donate `x` by reference (zero copy); `x` is consumed (using it afterward is a
  compile error). The owned buffer comes back only through a return value.
- **`copy x`** — hand over a *duplicate* (a memcpy); the caller keeps its original. (`copy` of an
  `opaque` is rejected — it is non-copyable.)

A bare name passed to an `own` parameter is a compile error
(`value 'x' must be moved or copied into 'own' parameter …`). This is the fill-buffer / FFI shape —
an `own` buffer is filled and handed back:

```arche
func read_into(fd: int, own buf: char[], len: int) -> (char[], int) {
  n := read(fd, buf, len);                 // the extern fills the owned buffer
  return buf, n;
}

buf: char[256];
buf,   n := read_into(0, move buf, 256);   // zero copy: buf threaded in and back out, in place
other, n := read_into(0, copy buf, 256);   // buf kept; `other` is a fresh, filled duplicate
```

Because a function only ever mutates storage it **owns**, *in-place vs. fresh is the caller's
decision*: `buf := f(move buf)` reuses `buf`'s storage with **no copy at all** (the buffer pointer
threads straight through and is returned), while `out := f(copy buf)` leaves `buf` intact. The
function is identical either way.

- **You can't move out of a borrow.** `move`-ing a borrowed (non-`own`) array parameter is a
  compile error — it would let a callee mutate the caller's buffer. You *may* `copy` a borrow.
- **Must-consume:** an opaque *local* must leave its scope before the end — moved into an `own`
  parameter, returned, or `insert`ed into a pool. Otherwise it is a compile error
  (`opaque value 'w' not consumed before scope end`). No implicit `drop`/RAII, no silent leak.

```arche
proc render() {
  w := window_open("demo", 640, 480);
  window_present(w, fb, 640, 480);   // plain read-only borrow
  window_close(move w);              // `own` param consumes it — w dead afterward
}
```

## Multi-Value Let

The `a, b, … := function()` syntax captures the multiple return values of a multi-return
function (see above):

```arche
s, d := sum_diff(10, 3);          // bind both returns
x, y, z := some_func();           // bind all returns
```

Targets bind **left-to-right** in the function's return-type order. A target may be a new `x:` or
an existing variable. A filled buffer is **returned** like any other value, so it is part of the
multi-bind — `buf, n := read_into(0, move buf, 256)` rebinds `buf` and binds the count, with no
hidden side effect.

## Status

🚧 **Alpha: Core infrastructure working**

### What's Working

- **Lexer**: Tokenizes source into language constructs (keywords, identifiers, operators)
- **Parser**: Builds AST for archetypes, procedures, systems, functions, expressions, multi-value bindings
- **Semantic Analysis**: Symbol table, scope tracking, type checking, field validation, multi-value binding
- **Code Generation**: Compiles to LLVM IR, assembles, and links to executables
- **Functions**: User-defined and extern functions with return values
- **Procedures**: User-defined and extern procedures (void)
- **Systems**: Data-driven transformations over matching archetypes
- **For loops**: Infinite loops, condition-based loops, range iteration
- **External Functions**: C function calls with copy semantics
- **Multi-return signatures**: `-> (T1, …, Tn)` — genuine multi-value return (aggregate ABI), destructured by a multi-bind
- **Multi-value Let**: Capture multiple return values from function calls
- **Archetype Operations**: Allocation, indexing, column access, tuple columns, shaped arrays
- **C Stdlib Interop**: File I/O via C stdlib wrappers (fopen, fread, fwrite, fclose)
- **Real-world ETL**: CSV loading and data processing benchmarks

### Known Limitations

- No error recovery in parser
- Limited standard library (basic syscalls only)

### Design Choices (Not Limitations)

- No string type — users implement fixed-size char arrays or string handling in archetypes
- No dynamic arrays — fixed-size allocations only (enforces predictable memory planning)
- No generic/polymorphic functions — keep types and semantics explicit
- No pointers or references — data organized in columnar archetypes instead
- No complex nested types — all data is columnar and flat
