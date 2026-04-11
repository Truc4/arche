# Arche — Language Specification

## Core Concepts

### Pool

A pool is a named collection of entries with a fixed schema.

```
pool Name {
  field1: Type,
  field2: Type,
}
```

- Each pool stores zero or more entries
- All entries in a pool have identical structure
- Pools are the only persistent storage

### Entry

An entry is a single instance of a pool's schema.

- Entries are created via allocation
- Entries are identified by handles
- Entries do not contain nested persistent objects

### Handle

A handle is a typed reference to an entry in a specific pool.

- `T` in a field position refers to `handle<T>`
- Handles are not memory addresses
- Handles are the only way to reference persistent data

**Example:**
```
pool Body {
  pos: Position,  // handle<Position>
}
```

### Field

A field is a named component of a pool entry.

Field types can be:
- Primitive types (`i32`, `f32`, etc.)
- Handles to other pools

## Operations

### Allocation

Allocation creates a new entry in a pool.

```
let h = Pool.alloc(field1: value, field2: value);
```

- Returns a handle to the new entry

### Deallocation

Deallocation removes an entry from a pool.

```
Pool.free(handle);
```

- Invalidates the handle

### Procedure

A procedure is an executable block.

```
proc name() {
  ...
}
```

- Does not return a value
- Used to read and mutate pool data

### Iteration

Iteration ranges over all entries of a pool.

```
for x in Pool {
  ...
}
```

- `x` is a handle to a pool entry

### Field Access

Field access retrieves data from an entry.

```
x.field
```

- If `x` is a handle, access resolves through its pool
- If the field is a handle, the result is a handle

### View (implicit)

A view is a temporary binding to a pool entry derived from a handle.

```
let pos = b.pos;
```

- `pos` provides field access to the referenced entry
- Does not copy data

### Assignment

Assignment updates a field of an entry.

```
pos.x = 1.0;
```

- Mutates the underlying pool entry

## Type System

### Primitive Types

Basic value types:

- `i32`, `f32`, `bool`, ...
- Stored directly in pool fields or locals

## Rules

### Persistence Rule
All persistent structured data must be stored in pools.
No persistent data exists outside pools.

### Reference Rule
All references to persistent data are handles.
No pointers or raw memory references exist.

### Composition Rule
Pools do not contain other pools inline.
Relationships between pools are expressed only through handles.

## Execution Model

Programs execute procedures.
Procedures operate by iterating pools and mutating entries.

## Example

```
pool Position {
  x: f32,
  y: f32,
}

pool Velocity {
  dx: f32,
  dy: f32,
}

pool Body {
  pos: Position,
  vel: Velocity,
}

proc update() {
  for b in Body {
    let pos = b.pos;
    let vel = b.vel;

    pos.x = pos.x + vel.dx;
    pos.y = pos.y + vel.dy;
  }
}
```
