# Devices and drivers

Arche's library model is built on a hardware metaphor: **everything is a device or a driver.**

- A **device** is a library — a group of files that defines *shapes* (archetypes) and *systems* (behavior). A device declares what it operates on but, ideally, does not allocate storage.
- A **driver** is the program that uses devices — it **picks the storage and its size**, drives the device's systems, and has the ultimate say. The top-level program is the driver.

Groups all the way up: **files → device → driver.**

## Importing: a device by name, a module by path

You **import a device by name** and a **plain module by path**. A bare name in `#import { … }` must
resolve to a *device* — a folder unit that ships a `.ds.arche` datasheet. A plain module (no datasheet)
is imported by a string path:

```arche
#import { router net "./util" }   // router, net = devices (by name); ./util = a plain module (by path)
```

Importing a non-device by bare name is an error that points you at the path form. `core` (the prelude)
is exempt — it is prepended, never imported.

**Namespacing is device-only.** A device's pure-Arche exports are namespaced — you call `net.listen`,
`fmt.assert` (a dot at a call site means you crossed a contract boundary). A **plain module merges
flat** (Jai `#load` style): its decls land *unprefixed* in the importer, so `#import { "./util" }`
then `helper()`, not `util.helper()`. Rationale: a plain module is structurally dev-internal (path
import + no datasheet = not a publishable lib), so it has no contract to namespace — it's just your
own program split across files. Name collisions across flat-merged files are a hard error (E0117).
(A device's `#foreign` externs keep their bare C name; only a device's pure-Arche decls get the `device.` prefix.)

**Every stdlib module is a device** (`net io http str router csv fmt parse os term`) — each ships a
`.ds.arche`, so all are imported by name.

## A device's impl is behavior-only (rule 3)

A device's impl (`.arche`) holds **behavior** — procs, funcs, systems. Its **types** (opaque, enums,
aliases, archetype shapes) and **storage requirements** live in its `.ds.arche` datasheet; the device
**may not allocate a pool** (the driver owns storage). Defining a type/archetype or allocating in a
device impl is an error. (Types are global shared vocabulary — `net`'s `socket` is the type `socket`,
written bare, not `net.socket`.)

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

Three bands (file ⊂ unit ⊂ exported): `#module` is a banner marking the decls below it **unit-private** — visible across the unit's files but hidden from importers. `#file` is strictly narrower: **file-local** — visible only within its own file, NOT to sibling files of the same unit (each file's `#file` decls are renamed to a file-unique identity, so a sibling's bare reference can't bind to them). Default (no banner) is exported.

## Testing a device

A device is tested by writing a **driver for it**, inline as a doctest: the doctest sizes the device's shapes, drives its systems, and asserts results. Doctests run under `arche test` and never reach a real driver's build.

## The datasheet (`.ds.arche`) and `@implements`

A device can declare the **components it requires** in a `<name>.ds.arche` *datasheet* — these are shared **global** vocabulary (registered unprefixed), so the driver references them by bare name and builds its own shape from them. The device's systems then bind to the driver's shape by column name:

```arche
// physics/physics.ds.arche   — datasheet: the components physics requires
pos :: float
vel :: float
```

```arche
// physics/systems.arche      — physics owns no shape, no storage
integrate :: sys (pos, vel) { pos = pos + vel; }
```

```arche
// main.arche                  — the driver owns the shape, built from physics's components
#import { physics }
Thing :: arche { pos, vel, mass :: float }   // references the datasheet's pos/vel + its own mass
Thing[1000]
// run physics.integrate over Thing
```

If the driver wants to call a required component by a **different name**, it binds with `@implements`:

```arche
@implements(physics.foo)
bar :: float                  // bar IS physics's foo; physics's systems are rewritten foo -> bar
```

A `run device.system` where no shape provides the system's components is a build error (not a silent no-op).

## Storage requirements (datasheet minimums) and `arche fill`

A datasheet may also state a **storage requirement** — the minimum number of rows the driver must
provide for one of the device's shapes. A pool decl inside a `.ds.arche` is a *requirement*, not an
allocation: it emits no storage; it records a minimum the driver's own pool must meet.

```arche
// store/store.ds.arche   — datasheet: store's shape + its minimum storage
Node :: arche { key :: float, val :: float }
Node[4]                    // REQUIREMENT: the driver must size Node to at least 4 rows
```

```arche
// main.arche             — the driver owns the pool and sizes it (>= the minimum)
#import { store }
Node[8]                    // sized above the minimum; the device's systems run over this pool
```

Rules (the driver owns all storage; the datasheet only states minimums):

- A driver pool smaller than the datasheet minimum is an error (`… requires >=N rows …`).
- A required shape with **no** driver pool is a hard error (`no storage for … — run \`arche fill\` …`).
- When two devices require the **same shape** (shapes are shared by structure), the minimums compose
  by `max` and the build notes the shared pool. Identical datasheet vocabulary (components/shapes)
  across devices dedups silently — declaring it twice is sharing, not a redefinition.
- The shape may be written inline (`Node :: arche { key :: float, val :: float }`) or with the
  components pre-defined first (`key :: float` / `val :: float` then `Node :: arche { key, val }`).
  Both forms work.

**`arche fill <driver>`** reads the datasheets of the devices a driver imports and writes a pool decl
for each required shape the driver doesn't already size, at the minimum — into *your* source, where
you edit it. It is idempotent (a shape already sized is left untouched). `arche init driver <name>
<device>...` scaffolds a driver importing the devices and runs the same fill.

Note: a device that declares its shape in its datasheet can't drive that shape from its **own**
doctest — doctests run in single-file context and don't merge the sibling `.ds.arche`. A device is
exercised by a driver (or the doctest declares its own local shape).

## Status

Fully implemented and tested (`tests/unit/language/devices/`): qualified pool decls, anonymous-literal unification, cross-module `run device.system`, `#module` visibility, the `.ds.arche` datasheet, `@implements` name-mapping, the unsatisfied-`run` diagnostic, datasheet storage requirements (minimums + composition + enforcement, `tests/unit/language/devices/storage/`), and `arche fill`. See `docs/DEVICE_DRIVER_DECISIONS.md` for the design decisions.
