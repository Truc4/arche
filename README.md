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
- **Two kinds of data**:
  - **Columns** (arrays of values, one per element)
  - **Metadata** (single values describing the whole collection)
- **Minimal type system**: No booleans, no objects.
- **Explicit structure over flexibility**

This leads to a style that feels closer to **data pipelines** or **vectorized computation** than traditional imperative code.

## Worlds

A World is a collection of archetypes. Define a world first:

```arche
world GameWorld()
```

Worlds are sparse arrays that efficiently handle entity creation and deletion while preserving handles.

## Archetypes (`arche`)

An Archetype is a type definition for structured data. It contains **aligned arrays (columns)** of field values.

Define an archetype (a reusable template):

```arche
arche Particle {
  pos: (x: float, y: float),
  vel: (vx: float, vy: float),
  mass: float
}
```

Allocate a fixed-size collection once:

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

## No Implicit Row Access

You **cannot** do this:

```arche
player[i]  // ❌ not allowed
```

Instead, you must be explicit:

```arche
particles.position[i]
particles.velocity[i]
```

This keeps the language focused on whole-array transformations.

## Indexing

Indexing works only on columns. For tuple columns, access by label:

```arche
particles.pos.x[i]
particles.pos.y[i]
particles.vel.vx[i]
particles.vel.vy[i]
```

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

Systems perform **data-driven transformations** over all matching archetypes in a world.

```arche
sys move(pos, vel) {
  pos = pos + vel;
}

proc update() {
  run move in GameWorld;
}
```

### Semantics

- executes via `run system_name` statement (operates on default world)
- or `run system_name in world_name` for explicit world selection
- automatically matches any archetype in that world containing the required fields
- binds those fields inside the system body
- operates on whole columns (array-first)

This means the system applies to any archetype in the world with `pos` and `vel`, such as:

- `Player`
- `Mob`
- `Projectile`

without needing to reference them explicitly.

### Execution

Systems are invoked explicitly within procedures:

```arche
proc main() {
  run move in Simulation;
  run dampen in Simulation;
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

// Create a world
world Simulation()

// Allocate and run systems
proc main() {
  let particles = alloc Particle(10000);
  run move in Simulation;
  run dampen in Simulation;
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
