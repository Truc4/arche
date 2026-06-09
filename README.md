# Arche

**Arche** is a small, experimental, array-first programming language: you operate on
_collections of structured data_ as a whole, not one element at a time. Data lives in
columnar **archetypes**, memory is **statically allocated** upfront, there are **no
pointers**, and whole-column operations compile through **LLVM** to native code.

> Programs should operate on collections of structured data as a whole, not one element at a time.

It is intentionally **minimal**, **opinionated**, and **not for production**. This is a
data-oriented-design / ECS exploration - basically a playground for the idea.

**Editor support:** [Truc4/arche.nvim](https://github.com/Truc4/arche.nvim) - a Neovim plugin / LSP client.

**Libraries — devices & drivers:** Arche organizes reusable code as *devices* (a group of files
defining shapes + systems) and *drivers* (programs that size the storage and run them). See
[docs/devices.md](docs/devices.md).

## Why Arche?

Arche is a playground for **data-oriented design (DOD)** as a language, not a pattern you
hand-roll on top of C++. The premise: structure a whole program the way a database structures
data - columnar tables you transform in bulk - and see how far it goes when that is the _only_
way to write code.

- **Data-oriented by default** - think in columns and whole-collection transforms, not objects and element loops.
- **Database-style data model** - archetypes are tables defined by a set of component types; a system is a query that runs over every matching table.
- **No heap, ever** - all storage is static and planned upfront, so memory behavior is fully predictable.
- **Functional meets procedural** - pure `func` values alongside procedural static operations (`proc` / `sys`).
- **Crashes are opt-in and visible** - the rare op that can still fail at runtime (an out-of-bounds index, a full pool) carries a *failure policy* right at the site: `a[i] !clamp`, `n / d !zero`, `a[i] !undefined`. A `func` is total by construction — it can **never** crash — and a `proc` crashes only at an explicit `!abort`; only `!abort` can ever abort, so `--no-abort` proves a whole build crash-free.

## Requirements

Building and running Arche programs needs an **LLVM + C toolchain** on your `PATH`:

- `opt` and `llc` — from a full LLVM install (the `llvm-libs` pulled in by `clang` is **not**
  enough; you need the standalone tools).
- `cc` — a C compiler/linker (`clang` or `gcc`).

Install the LLVM tools and a C compiler with your package manager — for example, on Arch Linux
they're in the official `extra` repo (not the AUR): `sudo pacman -S llvm clang`. On Debian/Ubuntu:
`sudo apt install llvm clang`.

## Quick start

```sh
git clone https://github.com/Truc4/arche && cd arche
make                  # builds ./build/arche
sudo make install     # optional: put `arche` on your PATH (PREFIX=<dir> for a custom location)
```

The installed `arche` is relocatable — it finds its standard library and runtime relative to the
binary.

```sh
echo 'proc main() { print("Hello, World!\n"); }' > hello.arche
arche run hello.arche                # compile + run     -> Hello, World!
arche build hello.arche -o hello     # …or build a binary
./hello                              # -> Hello, World!
```

(If you skipped `make install`, use `./build/arche` in place of `arche`.)

## A taste of Arche

```arche
arche Particle {
  pos :: float,
  vel :: float,
}

Particle[100]

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

## Command-line interface

```
arche build <file>      compile to an executable       arche fmt <files>      format (--check / --write)
arche run <file>        compile and run                arche check <file>     type-check only (no output)
arche test <paths>      run doctests                   arche explain <code>   long-form diagnostic help
arche analyze [--serve] language-server analysis       arche completion <sh>  bash | zsh | fish
```

`arche --help` or `arche help <command>` lists every flag; `arche build --emit=<kind>` emits
`llvm-ir`, `asm`, or `obj` instead of a linked executable.

Shell completion (also installed by `make install`):

```sh
arche completion bash | sudo tee /usr/share/bash-completion/completions/arche
arche completion zsh  > ~/.zsh/completions/_arche      # ensure the dir is on $fpath
arche completion fish > ~/.config/fish/completions/arche.fish
```

## Installing system-wide

`make install` (default `PREFIX=/usr/local`, override for a rootless install) lays out a
relocatable tree:

| Path | Contents |
|------|----------|
| `$PREFIX/bin/arche` | the compiler |
| `$PREFIX/bin/arche-analyzer` | language-server backend (also `arche analyze`) |
| `$PREFIX/lib/arche/{core,stdlib,runtime,explain}` | prelude, modules, runtime objects, diagnostics |
| `$PREFIX/share/{bash-completion,zsh,fish}/…` | completion scripts |

Resource discovery order: `$ARCHE_SYSROOT` → per-resource env overrides (`ARCHE_CORE_DIR`, …) →
the exe-relative install layout → the in-tree build paths — so an uninstalled `build/arche` also
works in place with no configuration.

## Experimental features

Each is covered in depth in the [language reference](docs/language.md).

- **Static allocation only (no heap)** - `T[N]` pools with free-lists; zero `malloc` in
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
