# Arche

**Arche** is a small, experimental programming language built around a single idea:

> Programs should operate on _collections of structured data_ as a whole — not one element at a time.

It is intentionally **minimal**, **opinionated**, and **not designed for production use**.
This project exists primarily for exploration.

## Philosophy

Arche is built on a few strong constraints:

- **Array-first**: Operations apply across entire collections by default.
- **No implicit row access**: You don’t work on individual elements unless you explicitly index.
- **Two kinds of data**:
  - **Columns** (arrays of values)
  - **Metadata** (single values describing the whole collection)

- **Minimal type system**: No booleans, no complex object system (yet).
- **Explicit structure over flexibility**

This leads to a style that feels closer to **data pipelines** or **vectorized computation** than traditional imperative code.

## Archetypes (`arche`)

An Archetype or `arche` is the primary data structure.
It is a collection of **aligned arrays (columns)** plus **metadata**.

```arche
arche Player {
  meta drag: Float
  col pos: Vec3
  col vel: Vec3
}
```

- `meta` fields: one value for the entire collection
- `col` fields: one value per element

## Array-Oriented Operations

Operations on columns apply across the entire collection:

```arche
players.pos += players.vel
players.vel *= players.drag
```

This is equivalent to looping over every element — but expressed as a single operation.

## No Implicit Row Access

You **cannot** do this:

```arche
player[i]  // ❌ not allowed
```

Instead, you must be explicit:

```arche
players.pos[i]
players.vel[i]
```

This keeps the language focused on whole-array transformations.

## Indexing

Indexing works only on columns or arrays:

```arche
players.pos[i]
grid[x, y, z]
```

Multidimensional indexing uses comma-separated indices.

## Numeric Model

- Only numeric primitives (no `Bool` type)
- Comparisons produce numeric values (`0` or `1`)
- Conditions treat `0` as false, non-zero as true

```arche
x = a < b   // x is 0 or 1
```

## Procedures (`proc`)

Procedures perform **explicit operations**.

```
proc init {
  players = alloc Player(100)
}
```

- run once
- operate on explicitly referenced data
- used for setup, orchestration, or control flow
- not data-driven

## Systems (`sys`)

Systems perform **data-driven transformations** over all matching archetypes.

```
sys move(pos, vel) {
  pos += vel
}
```

### Semantics

- runs once per matching archetype
- automatically matches any archetype containing the required fields
- binds those fields inside the system body
- operates on whole columns (array-first)

This means the system applies to any archetype with `pos` and `vel`, such as:

- `Player`
- `Mob`
- `Projectile`

without needing to reference them explicitly.

## Example

```
arche Particle {
  meta drag: Float
  col pos: Float
  col vel: Float
}

sys move(pos, vel) {
  pos += vel
}

sys damp(vel, drag) {
  vel *= drag
}
```

## Functions (`func`)

Functions are **pure computations** and do **not** mutate arche data.

```
func drag_factor(x: Float) -> Float {
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
  particles.vel *= (particles.pos > 10)
}
```

## What Arche Is _Not_

- Not a general-purpose language (yet)
- Not optimized for performance (yet)
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

🚧 **Work in progress**

The language, compiler, and design are all evolving.
Expect breaking changes, missing features, and rough edges.

## Final Note

Arche is not trying to be practical.

It’s trying to be **clear**, **small**, and **interesting**.

If it makes you think differently about data and computation, it’s doing its job.

```

```
