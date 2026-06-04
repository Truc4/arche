# Devices and drivers

Arche's library model is built on a hardware metaphor: **everything is a device or a driver.**

- A **device** is a library — a group of files that defines *shapes* (archetypes) and *systems* (behavior). A device declares what it operates on but, ideally, does not allocate storage.
- A **driver** is the program that uses devices — it **picks the storage and its size**, drives the device's systems, and has the ultimate say. The top-level program is the driver.

Groups all the way up: **files → device → driver.**

## Caller-sized pools (qualified pool decls)

A device defines a shape but leaves the pool to the caller. The driver sizes the device's shape by its **qualified name**:

```arche
// physics/physics.arche  — the device: a shape + a system, no pool
Particle :: arche { pos :: float, vel :: float }

integrate :: sys (pos, vel) {
  pos = pos + vel;
}
```

```arche
// main.arche  — the driver: sizes the device's shape and runs its system
#import { physics fmt }

physics.Particle[1000]              // the driver picks the size

main :: proc() {
  insert(physics.Particle, 10.0, 1.0);
  run physics.integrate;            // run the device's system by qualified name
  fmt.printf("pos0 = %d\n", physics.Particle.pos[0] * 10);  // 110
}
```

- `physics.Particle[N]` — a **qualified pool decl** sizes an imported device's shape. Different drivers can size the same device's shape differently.
- `run physics.integrate;` — a driver runs an imported device's system by **qualified name**.
- A shape may have **at most one pool** program-wide; two pool decls for the same canonical shape are a compile error ("Shape already allocated").

## Shapes are structural; names are aliases

A shape is identified by its **set of component (field) names**, not by its name. So a named shape and an anonymous **shape literal** with the same fields are the *exact same* canonical shape — one struct, one pool:

```arche
Foo :: arche { x :: float, y :: float }
Foo[4]
insert(arche { x :: float, y :: float }, 1.0, 2.0);  // inserts into Foo's pool
```

Components are **program-global** and defined exactly once; reference a component elsewhere by its bare name.

## Visibility

`#plugin` (the device/driver spelling of `#module`) is a banner region marking the decls below it as **device-private** — visible across the device's files, hidden from importers. `#file` narrows to file-local.

## Testing a device

A device is tested by writing a **driver for it**, inline as a doctest: the doctest sizes the device's shapes, drives its systems, and asserts results. Doctests run under `arche test` and never reach a real driver's build.

## Status / roadmap

Implemented and tested (`tests/unit/language/devices/`): qualified pool decls, anonymous-literal unification, cross-module `run device.system`, `#plugin`.

Designed, not yet implemented (see `docs/DEVICE_DRIVER_DECISIONS.md` and `tests/unit/language/known_failures/implements_binding.arche`): the **`.i.arche` datasheet** (a device declaring required components with no owned shape) and the **`@implements` decorator** (a driver binding its *own* differently-named type to a device requirement). These require global cross-module component identity and are a deliberate follow-up.
