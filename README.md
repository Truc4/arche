# Arche

**Arche** is a small, experimental programming language built around a single idea:

> Programs should operate on _collections of structured data_ as a whole — not one element at a time.

It is intentionally **minimal**, **opinionated**, and **not designed for production use**.
This project exists primarily for exploration.

It's basically just my silly DOD and ECS concepts exploration. I have no idea what I'm doing.

## Philosophy

Arche is built on a few strong constraints:

- **Array-first**: Operations apply across entire collections by default.
- **No implicit row access**: You don’t work on individual elements unless you explicitly index.
- **Columns, not fields**: All archetype data is columnar (arrays). Single values are metadata stored in the archetype itself.
- **Tuple columns**: Multi-component fields like position vectors are stored as separate side-by-side arrays (`pos_x`, `pos_y`) but accessed with clean syntax (`pos.x[i]`, `pos.y[i]`).
- **Fixed-size collections**: Archetypes allocate once with a fixed count. No dynamic resizing.
- **Minimal type system**: Primitives only: `int`, `float`, `char`. No booleans, no objects.
- **Explicit structure over flexibility**

This leads to a style that feels closer to **data pipelines** or **vectorized computation** than traditional imperative code.

## Worlds (Planned, Not Yet Implemented)

A **World** is a planned feature that will act as a collection of archetypes and a scope for systems. The syntax would be:

```arche
world Simulation()
```

Multiple worlds will allow parallel data-driven computations, with systems operating on all matching archetypes within a specific world. **This feature is not yet implemented.** Currently, systems operate on all matching archetypes in the scope.

## Archetypes (`arche`)

An Archetype defines the structure of a columnar data collection. All fields become arrays (columns), except scalar metadata stored in the struct itself.

**Primitives:** `int`, `float`, `char`

**Field types:**
- Single primitive: `mass: float` → scalar metadata in archetype struct
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

**Allocate once, fixed size:**

```arche
let particles = alloc Particle(1000);
```

All 1000 slots are live immediately. No dynamic resizing.

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
- Not optimized for performance
- Not stable
- Not feature-complete

It deliberately avoids:

- classes / inheritance
- implicit iteration
- complex type systems
- hidden behavior

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

🚧 **Alpha — Core infrastructure working**

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
- Type system is minimal (primitives only)
- No error recovery in parser
- Limited standard library (only `write` syscall)
