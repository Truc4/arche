# Arche

**Arche** is a small, experimental, array-first programming language: you operate on
_collections of structured data_ as a whole, not one element at a time. Data lives in
columnar **archetypes**, memory is **statically allocated** upfront, there are **no
pointers**, and whole-column operations compile through **LLVM** to native code.

> Programs should operate on collections of structured data as a whole, not one element at a time.

It is intentionally **minimal**, **opinionated**, and **not for production**. This is a
data-oriented-design / ECS exploration - basically a playground for the idea.

**Editor support:** [Truc4/arche.nvim](https://github.com/Truc4/arche.nvim) - a Neovim plugin / LSP client.

## Why Arche?

Arche is a playground for **data-oriented design (DOD)** as a language, not a pattern you
hand-roll on top of C++. The premise: structure a whole program the way a database structures
data - columnar tables you transform in bulk - and see how far it goes when that is the _only_
way to write code.

- **Data-oriented by default** - think in columns and whole-collection transforms, not objects and element loops.
- **Database-style data model** - archetypes are tables defined by a set of component types; a system is a query that runs over every matching table.
- **No heap, ever** - all storage is static and planned upfront, so memory behavior is fully predictable.
- **Functional meets procedural** - pure `func` values alongside procedural static operations (`proc` / `sys`).

## Quick start

```sh
make                                              # build the compiler -> build/arche
./build/arche examples/hello_world/hello_world.arche -o hello
./hello                                           # -> Hello, World!
```

```sh
./build/arche examples/archetype/test_archetype.arche -o demo && ./demo
make test                                         # run the full test suite
```

## A taste of Arche

```arche
arche Particle {
  pos :: float,
  vel :: float,
}

static pool<Particle>(100);

sys integrate(pos, vel) {
  pos = pos + vel;        // runs over the whole column, no explicit loop
}

proc main() {
  insert(Particle, 1.0, 0.1);
  run integrate;
  print("systems executed\n");
}
```

A `sys` automatically matches any archetype carrying the components it names; `run` executes
it over every matching column. See the [language reference](docs/language.md) for archetypes,
the `proc`/`func`/`sys` split, ownership, and more.

## Experimental features

Each is covered in depth in the [language reference](docs/language.md).

- **Static allocation only (no heap)** - `static pool<T>(N)` with free-lists; zero `malloc` in
  hot loops, no fragmentation, no use-after-free.
- **Database-style archetypes** - a shape is a _set_ of component types, so `{a, b}` and
  `{b, a}` are the same table and share one pool; columns are reached by type name, not field
  name.
- **`func` / `proc` / `sys`** - a pure functional value, a procedural action, and a data
  transform over tables; the distinction is enforced by the grammar itself.
- **Ownership without a GC or borrow-checker zoo** - read-only borrow by default; opt into
  `own` + caller `move` (zero-copy) / `copy` (memcpy).
- **Implicit loops** - whole-column ops have no element loop in source; the compiler generates
  it.

## Documentation

- [Language reference](docs/language.md) - memory model, types, archetypes, `proc`/`func`/`sys`, ownership, foreign resources
- [Performance](docs/performance.md) - benchmarks and what drives them
- [Tooling](docs/tooling.md) - CLI, editor/LSP, build targets, diagnostics
- [Doc comments & doctests](docs/DOCTESTS.md)
- [Grammar](docs/GRAMMAR.peg) - the formal PEG grammar
- [Design analysis](design_analysis/README.md) - layout experiments and full cross-engine benchmarks

## Status

🚧 **Alpha - core infrastructure working.**

Working: lexer, parser, semantic analysis (symbol table, scopes, type checking), LLVM codegen
to native executables, `func`/`proc`/`sys` with the grammar-enforced split, `extern proc` C
FFI, archetype allocation / indexing / column + tuple access, proc out-params and zero-copy
in-out, for-loops, C stdlib file I/O, and a real CSV ETL path.

Known limitations: no parser error recovery; a small standard library.

## What Arche is _not_

Not general-purpose, not production-tuned, not stable, not feature-complete. It deliberately
avoids pointers, dynamic memory, classes/inheritance, implicit iteration, complex type
systems, and hidden behavior - see [What it avoids](docs/language.md#design-priorities).
