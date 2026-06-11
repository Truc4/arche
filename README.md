# Arche

**Arche** is a small, experimental, array-first programming language: you operate on
_collections of structured data_ as a whole, not one element at a time. Data lives in
columnar **archetypes**, memory is **statically allocated** upfront, there are **no
pointers**, and whole-column operations compile through **LLVM** to native code.

> Programs should operate on collections of structured data as a whole, not one element at a time.

It is intentionally **minimal**, **opinionated**, and **not for production**. This is a
data-oriented-design / ECS exploration - basically a playground for the idea.

**Editor support:** [Truc4/arche.nvim](https://github.com/Truc4/arche.nvim) - a Neovim plugin / LSP client.

## Quick start

> Needs an **LLVM + C toolchain** on your `PATH` — see [Requirements](#requirements) just below.

```sh
git clone https://github.com/Truc4/arche && cd arche
make                  # builds ./build/arche
sudo make install     # optional: put `arche` on your PATH (PREFIX=<dir> for a custom location)
```

The installed `arche` is relocatable — it finds its standard library and runtime relative to the
binary. (If you skipped `make install`, use `./build/arche` in place of `arche`.)

```arche
// hello.arche
#import { fmt }

main :: proc() {
  fmt.print("Hello, World!\n")();
}
```

```sh
arche run hello.arche                # compile + run     -> Hello, World!
arche build hello.arche -o hello     # …or build a binary
./hello                              # -> Hello, World!
```

## Requirements

Building and running Arche programs needs an **LLVM + C toolchain** on your `PATH`:

- `opt` and `llc` — from a full LLVM install (the `llvm-libs` pulled in by `clang` is **not**
  enough; you need the standalone tools).
- `cc` — a C compiler/linker (`clang` or `gcc`).

Install the LLVM tools and a C compiler with your package manager — for example, on Arch Linux
they're in the official `extra` repo (not the AUR): `sudo pacman -S llvm clang`. On Debian/Ubuntu:
`sudo apt install llvm clang`.

## Why Arche?

Arche is a playground for **data-oriented design (DOD)** as a language, not a pattern you
hand-roll on top of C++. The premise: structure a whole program the way a database structures
data - columnar tables you transform in bulk - and see how far it goes when that is the _only_
way to write code.

- **Data-oriented by default** - think in columns and whole-collection transforms, not objects and element loops.
- **Database-style data model** - archetypes are tables defined by a set of component types; a system is a query that runs over every matching table.
- **No implicit heap** - all storage is static and planned upfront, so memory behavior is fully predictable. That's a property of the *language core*, **not** a ceiling on the data model: "no dynamic allocation" does **not** mean "no dynamic archetypes". **Dynamic (resizable) archetypes — the backbone of a full ECS — are on the roadmap as a library** built on the same columnar model, once the core language matures; the core just doesn't bake in implicit allocation.
- **Libraries as devices & drivers** - a *device* is a library that declares shapes + systems but owns no storage; the *driver* (your program) picks the pool sizes and runs the device's systems. A hardware metaphor for dependency injection: the storage owner is always the caller, never the library. See [docs/devices.md](docs/devices.md).
- **The "function," split four ways** - most languages overload one `function` keyword for jobs that have nothing in common. Arche gives each job its own form, and the grammar enforces the split (see below).
- **Crashes are opt-in and visible** - the rare op that can still fail at runtime (an out-of-bounds index, a full pool) carries a *failure policy* right at the site: `a[i] !clamp`, `n / d !zero`, `a[i] !undefined`. A `func` defaults to `clamp`, so an unannotated op in one can't crash; a `proc` defaults to `!abort`, the deliberate crash site. `--no-abort` bans every abort site (implicit or explicit) — but that is **not** a whole-build crash-free *proof*: `!undefined` is a raw unchecked op (UB can still fault), and a custom `policy` is your own code (it can call `_exit` or leave an op out of bounds). `--no-undefined` also bans the unsafe opt-out; the rest is on the policies you write.

### The "function," split four ways

A traditional `function` is asked to be a value, an effect, a loop, and an error handler all at once.
Arche refuses that overload: it splits the one keyword into **four distinct forms**, and the grammar
won't let you blur them. The shape of a declaration *tells you what it does*.

| Form     | Binding                     | Is a value? | Job |
| -------- | --------------------------- | ----------- | --- |
| `func`   | `name :: func(in) -> T`     | **yes**     | pure computation — one return, no side effects, **total by default** (unannotated ops clamp) |
| `proc`   | `name :: proc(in)(out)`     | no          | an action — does things; writes results into caller-provided out-params |
| `sys`    | `name :: sys(components)`   | no          | a data transform — runs over **every** archetype carrying those components |
| `policy` | `name :: policy(len, i)`    | no          | a failure macro — inlined at a fallible op (`a[i] !clamp`) to resolve the failure *at the site* |

```arche
area   :: func(w: int, h: int) -> int         // value:    r := area(w, h)
divmod :: proc(a: int, b: int)(q:, r:)        // action:   divmod(17, 5)(q:, r:)
step   :: sys(pos, vel) { pos = pos + vel; }  // transform: run step;
clamp  :: policy(len: int, i: int) { … }      // failure:  v := xs[k] !clamp
```

Why bother? Each split buys a real guarantee the overloaded keyword can't:

- **`func` vs `proc` — value vs effect.** A `func` is pure by construction (calling a proc/extern/
  mutating builtin from one is a hard error) and **total by default** (its baseline policy is
  `clamp`, so an unannotated fallible op can't crash). A proc is the only thing that *does*. So
  purity and the totality default are readable straight off the keyword — no annotations, no effect
  inference.
- **`sys` — the loop isn't yours to write.** A system names *components*, not tables, and the
  compiler runs it over every matching archetype, generating the column loop. Data-first iteration
  with no element loop in source.
- **`policy` — failure is a value-site decision, not a hidden default.** `xs[k] !clamp` puts the
  out-of-bounds behavior right on the op as an ordinary, user-writable macro. `!abort` is the
  deliberate crash site, and `--no-abort` bans every one — though that is not a full crash-free
  proof: `!undefined` (a raw unchecked op) and a misguided custom policy are still your own code.
  `--no-undefined` closes the unsafe opt-out.

## A taste of Arche

```arche
#import { fmt }

Particle :: arche {
  pos :: float,
  vel :: float,
}

Particle[100](100) { vel: 0.1 } // 100 live particles; pos starts at 0, vel at 0.1

integrate :: sys(pos, vel) {
  pos = pos + vel; // whole-column update — no explicit loop
}

main :: proc() {
  run integrate; // runs over every archetype carrying pos + vel
  fmt.print("stepped\n")();
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
  hot loops, no fragmentation, no use-after-free. (Scope note: this is the *core* language —
  *dynamic, resizable* archetypes for a full ECS are planned as a later library layer, not a
  language built-in. No implicit allocation ≠ no growth.)
- **Database-style archetypes** - a shape is a _set_ of component types, so `{a, b}` and
  `{b, a}` are the same table and share one pool; columns are reached by type name, not field
  name.
- **`func` / `proc` / `sys` / `policy`** - the overloaded "function" split four ways: a pure
  functional value, a procedural action, a data transform over tables, and a failure-handling
  macro; the distinctions are enforced by the grammar itself.
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
