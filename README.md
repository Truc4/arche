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
- **Tuple columns**: Multi-component fields like position vectors are stored as separate side-by-side arrays (`pos_x`, `pos_y`) but accessed with clean syntax (`pos.x[i]`, `pos.y[i]`).
- **Pooled allocation with free-lists**: `alloc Archetype(N)` pre-allocates capacity for N entries. Deleted slots are tracked in a free-list and reused by subsequent inserts, eliminating fragmentation.
- **Minimal type system**: Primitives only: `int`, `float`, `char`. No booleans, no objects. This is a design principle, not a limitation.
- **Explicit structure over flexibility**: Users have complete control and visibility over data layout in memory.

This leads to a style that feels closer to **data pipelines** or **vectorized computation** than traditional imperative code.

## Memory Model: Static Allocation Only

**Arche has NO dynamic or heap allocation.** All memory is allocated statically at program startup. This is a core design principle, not a limitation.

### Allocation Strategy

- **Explicit allocation**: `alloc Archetype(N)` declares a fixed-capacity allocation for archetype shape.
- **Static storage**: All allocations have program lifetime. They exist from program start to end. No deallocation during execution.
- **Fixed size**: Each allocation is immutable in size. No resizing, no growing vectors. Capacity is permanent once set.
- **Implicit cleanup**: At program end, all allocations are implicitly freed. Explicit deallocation is **not allowed** (and unnecessary).
- **Scope-implicit**: Scope (static vs world-scoped, when worlds exist) is determined by context where `alloc` appears.

### Allocation Initialization

Allocations can include field initialization to set all instances’ values at allocation time:

```arche
alloc Counter(5) { val: 7, score: 2.5 };
```

This sets all 5 instances’ `val` and `score` fields. Uninitialized fields are zero-initialized.

### Encouraged Pattern

Plan memory usage ahead of time, allocate what you need upfront, use predictably:

```arche
proc main() {
  // Allocate all archetype instances with fixed capacities upfront
  alloc Particle(10000) { active: 0 };
  alloc Enemy(5000);
  alloc Projectile(1000);

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

## Archetypes (`arche`)

An Archetype declaration defines a _shape_ (a unique column structure). The name is a handle to that shape. Declarations do **not** allocate space; only `alloc` does that.

A **shape** is defined by its columns and types. If two archetype declarations have identical columns and types, they are both handles to the same shape and will share an allocation if you allocate them.

**Primitives:** `int`, `float`, `char`

**Column types:**

- Single primitive: `mass: float` → column of floats
- Tuple columns: `pos: (x: float, y: float)` → two separate arrays `pos_x[]` and `pos_y[]`
- Multi-dimensional arrays: `text: char[256]` → N×256 array per entity

Example:

```arche
arche Particle {
  pos: (x: float, y: float),
  vel: (vx: float, vy: float),
  mass: float
}
```

This defines a shape with those columns. The name `Particle` is an alias. If you declare another archetype with the same columns:

```arche
arche Enemy {
  pos: (x: float, y: float),
  vel: (vx: float, vy: float),
  mass: float
}
```

Both `Particle` and `Enemy` are handles to the same underlying shape. Allocations using either handle reference the same shape. You can allocate using either handle:

```arche
alloc Particle(1000);
alloc Enemy(1000);  // ERROR: shape already allocated
```

**Static allocation:**

Each shape is allocated exactly once via an explicit `alloc Archetype(N)` call. Attempting to allocate the same shape again (with any alias) is a compile error.

The allocation is **fixed size** and **reallocation is discourages**. All capacity is allocated upfront. This is by design, dynamic memory is not a feature of this language.

## Array-Oriented Operations

Operations on archetype columns apply across the entire collection without explicit loops:

```arche
alloc Particle(1000);
// ... insert particles
Particle.pos = Particle.pos + Particle.vel;
```

This iterates all 1000 elements, updating each position by its velocity.

Inside a system function, the archetype parameter names are available directly:

```arche
sys move(pos, vel) {
  pos = pos + vel;
}
```

Same effect: iterate all elements element-wise.

## Indexing

Individual element access requires explicit column reference:

**Scalar columns:**

```arche
particles.mass[i]
```

**Tuple columns (labeled access):**

```arche
particles.pos.x[i]   // x component of position at index i
particles.pos.y[i]   // y component of position at index i
particles.vel.vx[i]  // x component of velocity at index i
```

**Multi-dimensional arrays:**

```arche
messages.text[i, j]  // 2D indexing
matrices.data[i, x, y]  // 3D indexing
```

This keeps the language focused on whole-array transformations. Most operations work on entire columns, not individual elements.

## Numeric Model

- Only numeric primitives: `int`, `float`, `char` (no `bool` type)
- Comparisons produce numeric values (`0` or `1`)
- Conditions treat `0` as false, non-zero as true

```arche
x = a < b   // x is 0 or 1
```

## Procedures (`proc`)

Procedures perform **explicit operations**.

```arche
proc main() {
  alloc Particle(1000);
  Particle.pos = Particle.pos + Particle.vel;
}
```

- run once
- operate on explicitly referenced data (archetype names are handles to allocated shapes)
- array ops apply to the whole collection
- used for setup, orchestration, or control flow

## Systems (`sys`)

Systems perform **data-driven transformations** over all matching archetypes.

```arche
sys move(pos, vel) {
  pos = pos + vel;
}

proc update() {
  run move;
}
```

### Semantics

- executes via `run system_name` statement
- automatically matches any archetype in scope containing the required fields
- binds those fields inside the system body
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

### Conditional Behavior (Planned Constraint)

Conditionals work through mathematical expressions (comparisons produce 0 or 1):

```arche
sys dampen(vel, pos) {
  // multiply velocity by 0 if below threshold
  vel = vel * (pos > 10);
}
```

**Planned:** Enforce that systems use only conditionals, disallow branching statements. This keeps systems pure data transforms, vectorizable, and cache-friendly.

## Example

```arche
// Define archetypes
arche Particle {
  pos: (x: float, y: float),
  vel: (vx: float, vy: float)
}

// Define systems that operate on matching archetypes
sys move(pos, vel) {
  pos = pos + vel;
}

sys dampen(vel) {
  vel = vel * 0.99;
}

// Allocate and run systems
proc main() {
  alloc Particle(10000);
  run move;
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

## Practical Example: ETL Workloads

Real-world benchmarks (1000 rows) show Arche's strength in data processing. ETL tasks (read CSV → transform → write) demonstrate vectorized column operations:

| Task | Operation | Arche | Pandas | Speedup |
|------|-----------|-------|--------|---------|
| Task1 | `revenue = price × quantity` | 0.857ms | 1.107ms | **1.29x** |
| Task2 | `valid = quantity > 0` | 0.775ms | 1.313ms | **1.69x** |
| Task3 | `bucket = price / 10` | 0.855ms | 1.297ms | **1.52x** |
| Task4 | `total = Σ(price × qty)` | 1.012ms | 1.091ms | **1.08x** |

Arche achieves consistent, predictable latency (low variance) while remaining competitive with interpreted data frameworks on small datasets. See `design_analysis/README.md` for full analysis.

**Limitations of this test**: 1000 rows is too small to measure batch optimization, Python startup overhead inflates Pandas time, no compiled baselines for fair comparison. **Future benchmarks** will test on 1M+ rows, compare against Polars/DuckDB/NumPy+Cython, measure throughput (rows/sec) and variance, and separate Python startup time. I assume these will not be as favorable to Arche, but will continue testing practical scenarios.

## Why This Exists

Arche is an experiment in:

- **data-oriented design**
- **language minimalism**
- **making constraints visible in syntax**
- exploring how far you can go with just:
  - arrays
  - primitives
  - structured grouping

## Out Parameters (`out`)

Out parameters allow functions to fill buffers and return them as values. When a parameter is marked `out`, the language allocates a buffer and returns it as part of the function result.

### Signature

Out parameters have no size specified in the type. Size is determined by parameters passed at call time:

```arche
extern func read(fd: int, out buf: char[], len: int) -> int;
extern proc write(fd: int, buf: char[], len: int);
```

### Pattern 1: Zero-initialized buffer (language allocates)

Don't pass a buffer; language creates one:

```arche
let buf, bytes_read = read(fd, 256);
```

The `out buf` argument is omitted. Language allocates a fresh 256-byte buffer on the stack, passes to C, returns it.

### Pattern 2: Copy-in semantics (you provide buffer)

Pass a buffer for the out parameter:

```arche
let new_buf, bytes_read = read(fd, old_buf, 256);
```

The buffer `old_buf` is passed. Language copies it in, passes to C, returns modified copy. Original `old_buf` unchanged.

### Copy Semantics

All parameters are always copied. C functions cannot modify caller data directly. Out parameters are returned as new values.

### Copy Semantics

Every function parameter is **always copied** — there are no side effects on the original data. Functions are pure with respect to parameters; side effects are explicit in the code.

## Multi-Value Let

The `let a, b, c = function()` syntax captures multiple return values from a single function call:

```arche
let buf, n = read(fd, buf, 256);     // Capture buffer and return value
let x, y, z = some_func();            // Capture multiple returns
```

Values are assigned **left-to-right** in signature order:

- Out parameters first (in order)
- Function return value last

Use `_` to discard values:

```arche
let buf, _ = read(fd, buf, 256);     // Discard return value, keep buffer
```

## Status

🚧 **Alpha: Core infrastructure working**

### What's Working

- **Lexer**: Tokenizes source into language constructs (keywords, identifiers, operators)
- **Parser**: Builds AST for archetypes, procedures, systems, functions, expressions, multi-value let
- **Semantic Analysis**: Symbol table, scope tracking, type checking, field validation, multi-value let binding
- **Code Generation**: Compiles to LLVM IR, assembles, and links to executables
- **Functions**: User-defined and extern functions with return values
- **Procedures**: User-defined and extern procedures (void)
- **Systems**: Data-driven transformations over matching archetypes
- **For loops**: Infinite loops, condition-based loops, range iteration
- **External Functions**: C function calls with copy semantics
- **Out Parameters**: Buffer allocation and return as values, with copy-in semantics
- **Multi-value Let**: Capture multiple return values from function calls
- **Archetype Operations**: Allocation, indexing, column access, tuple columns, shaped arrays
- **Basic I/O**: File operations via extern syscalls
- **CSV Loading**: Real-world example with file I/O and data parsing

### Known Limitations

- No error recovery in parser
- Limited standard library (basic syscalls only)

### Design Choices (Not Limitations)

- No string type — users implement fixed-size char arrays or string handling in archetypes
- No dynamic arrays — fixed-size allocations only (enforces predictable memory planning)
- No generic/polymorphic functions — keep types and semantics explicit
- No pointers or references — data organized in columnar archetypes instead
- No complex nested types — all data is columnar and flat
