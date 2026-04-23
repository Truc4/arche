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

**Arche has NO dynamic memory allocation.** This is a core design principle, not a limitation.

### Allocation Strategy

- **Explicit allocation**: Archetypes are allocated explicitly via `alloc Archetype(N)` calls at any point in the program.
- **Fixed size**: Each allocation is _fixed size_. There is no resizing, no growing vectors, no automatic expansion. Once allocated, capacity is permanent.
- **Deallocate implicitly**: At the end of the program, all allocations are implicitly deallocated. Explicit deallocation is **discouraged**.

### Encouraged Pattern

While allocations can happen anywhere, the encouraged design pattern is: **Plan your memory usage ahead of time, allocate what you need once early, use it predictably, and let it clean up automatically.**

This pattern encourages thinking about data layout early and prevents the hidden costs of dynamic allocation that plague real-time systems and data-intensive applications.

```arche
proc main() {
  // Allocate all archetypes upfront with capacities you need
  let particles = alloc Particle(10000);
  let enemies = alloc Enemy(5000);
  let projectiles = alloc Projectile(1000);
  
  // Run your program with this fixed memory budget
  run initialize;
  run update_loop;
  
  // Everything deallocates implicitly at end
}
```

### Why No Dynamic Memory?

- **Predictability**: Memory usage is known at program start.
- **Performance**: No allocation/deallocation overhead during hot loops.
- **Simplicity**: Language users reason about a fixed memory budget, not growth patterns.
- **Cache-friendly**: Fixed column layout enables effective prefetching and vectorization.

### If You Need More Space

If you truly need more capacity: deallocate the old archetype, reallocate a new one with larger capacity, and copy data. But this goes against the language’s spirit and is explicitly discouraged. **Plan better upfront instead.**

## Worlds (Planned, Not Yet Implemented)

A **World** is a planned feature that will act as a collection of archetypes and a scope for systems. The syntax would be:

```arche
world Simulation()
```

Multiple worlds will allow parallel data-driven computations, with systems operating on all matching archetypes within a specific world. **This feature is not yet implemented.** Currently, systems operate on all matching archetypes in the scope.

## Archetypes (`arche`)

An Archetype declaration allocates space for a _shape_ (a unique column structure). The name is just an _alias_ for that shape.

A **shape** is defined by its columns and types. If two archetype declarations have identical columns and types, they refer to the same shape and share the same allocation.

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

This allocates space for a shape with those columns. The name `Particle` is an alias. If you declare another archetype with the same columns:

```arche
arche Enemy {
  pos: (x: float, y: float),
  vel: (vx: float, vy: float),
  mass: float
}
```

Both `Particle` and `Enemy` alias the same underlying shape. They share the allocation. You can allocate using either alias:

```arche
let particles = alloc Particle(1000);
let enemies = alloc Enemy(1000);  // ERROR: shape already allocated
```

**Static allocation—once and only once:**

Each shape allocates exactly once via an explicit `alloc` call. Attempting to allocate the same shape again (with any alias) is a compile error.

The allocation is **fixed size** and **cannot be resized**. All capacity is allocated upfront. This is by design—dynamic memory is not a feature of this language.

## Array-Oriented Operations

Operations on archetype columns apply across the entire collection without explicit loops:

```arche
let particles = alloc Particle(1000);
particles.pos = particles.pos + particles.vel;
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
  let particles = alloc Particle(1000);
  particles.pos = particles.pos + particles.vel;
}
```

- run once
- operate on explicitly referenced data (via handles)
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
  let particles = alloc Particle(10000);
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

## Example: Conditional Behavior

```arche
proc damp {
  // multiply velocity by 0 if below threshold
  particles.vel = particles.vel * (particles.pos > 10);
}
```

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
- **Predictable memory usage**: Fixed allocations mean stable, foreseeable memory patterns—no garbage collection pauses, no dynamic resizing surprises.
- **Willing to use more memory**: Upfront allocation and column-oriented storage use more RAM than pointer-based designs, but the tradeoff is worth it for speed and predictability.

This is a deliberate choice: **prefer fast, predictable memory access over minimizing memory footprint.**

## For Language Developers

The `design_analysis/` directory contains exploration and documentation of data layout patterns that guide language development, implementation strategies, and future feature design.

## Why This Exists

Arche is an experiment in:

- **data-oriented design**
- **language minimalism**
- **making constraints visible in syntax**
- exploring how far you can go with just:
  - arrays
  - primitives
  - structured grouping

## Status

🚧 **Alpha: Core infrastructure working**

### What's Working

- **Lexer**: Tokenizes source into language constructs (keywords, identifiers, operators)
- **Parser**: Builds AST for archetypes, procedures, systems, functions, expressions
- **Semantic Analysis**: Symbol table, scope tracking, type checking, field validation
- **Code Generation**: Compiles to LLVM IR, assembles, and links to executables
- **Basic Examples**: hello_world, simple arithmetic, archetype definitions

### Known Limitations

- No function calls yet
- For loops not fully implemented
- No memory management beyond static allocation
- No error recovery in parser
- Limited standard library (only `write` syscall)
