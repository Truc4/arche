# Tooling

The compiler CLI, the editor analyzer/LSP, and the build system. For doc comments and
doctests see [DOCTESTS.md](DOCTESTS.md).

## Editor support: `arche.nvim`

[**Truc4/arche.nvim**](https://github.com/Truc4/arche.nvim) is the Neovim plugin / LSP
client. It surfaces, live as you type:

- **inlay hints** - inferred bind types, gopls-style parameter names (with an `own` marker on
  owned params), and a ghost `()` on bare `proc` calls
- **diagnostics** - errors and warnings as `vim.diagnostic` entries, with codes and notes
- **hover** - rendered doc comments

It drives `build/arche-analyzer` under the hood (see below).

## Compiler CLI

```
arche [flags] input.arche          # compile to an executable (implicit `build`)
arche build [flags] input.arche    # compile to an executable / artifact
arche run [flags] input.arche      # compile and run (forwards program args after `--`)
arche check [flags] input.arche    # analyze only; report diagnostics, write nothing (like `cargo check`)
arche test <path> [-v]             # run doctests (see DOCTESTS.md)
arche init device <name>           # scaffold a device folder (shape + system + doctest)
arche init driver <name> [dev...]  # scaffold a driver file; with devices, fill their required pools
arche fill <driver>                # size a driver's pools from its imported devices' datasheets
arche --explain <code>             # print the long-form explanation for a diagnostic
```

Compile flags:

- `-o <file>` - output executable path
- `-emit-llvm -o <file.ll>` - emit LLVM IR instead of an executable
- `--link <path>` - pass an extra `.c`/`.o` to `cc` at link time (repeatable)

Lint flags (accepted by `build`, `run`, and `check`):

- `-Wno-proc-could-be-func`, `-Wno-proc-no-effect`, `-Wno-large-stack-array` - disable a lint
- `-Werror=proc-could-be-func`, `-Werror=proc-no-effect`, `-Werror=large-stack-array` - promote one lint to an error
- `-Werror` - promote every enabled lint to an error
- `--exported-mutable <error|warn|allow>` - tune the exported-mutable-global lint (`W0022`, error by default)
- `--map-foreign-write <error|warn|allow>` - tune the map-writes-foreign-pool lint (`W0024`, error by default)
- `--forbid-allow` - reject any `@allow(...)` escape hatch in your code
- Any other lint is on by default and silenced per-declaration with `@allow(<slug>)` (e.g. `@allow(pointless_move)`).

`arche test` runs the runnable examples in `///` doc comments - see [DOCTESTS.md](DOCTESTS.md).
`arche --explain E0001` reads the long-form note from `docs/explain/<code>.md`.
`arche init` scaffolds the device/driver templates - see [devices.md](devices.md). It never
overwrites an existing file. `arche fill` writes a pool decl into a driver for each shape its imported
devices require (at the datasheet minimum) that the driver doesn't already size - idempotent, and
also run by `arche init driver <name> <device>...`. See [devices.md](devices.md#storage-requirements-datasheet-minimums-and-arche-fill).

## Diagnostics

Errors carry stable codes `E0001`–`E0220`; warnings `W0001`–`W0027` (promotable to
`-Werror`). Codes are stable forever (burn-on-delete) and severity is configuration, not identity —
a lint can be disabled (`-Wno-<slug>`), promoted (`-Werror=<slug>`), or silenced on one declaration
(`@allow(<slug>)`). A sample:

| Code | Meaning |
| ---- | ------- |
| `E0001` | undefined symbol |
| `E0002` | use after consume |
| `E0020` | opaque not consumed before scope end |
| `E0025` | `own` requires `move` or `copy` |
| `E0050` | action (proc/extern call) used in an expression |
| `E0090` | `func` not pure |
| `E0200` | type mismatch |
| `W0001` | proc could be a func |
| `W0002` | proc has no effect |
| `W0022` | exported mutable global (error by default) |
| `W0024` | map writes a foreign pool (error by default) |
| `W0026` | large stack array — prefer a pool or a sliced column |
| `W0027` | pointless `move` (e.g. of a pool column) |

Run `arche --explain <code>` for the full write-up.

## Editor analyzer

`build/arche-analyzer` is the editor-facing analysis service behind the LSP:

- `arche-analyzer --dump <file>` - one-shot: analyze and print every line (syntax tokens,
  doc comments, hints, diagnostics)
- `arche-analyzer --serve` - long-lived: driven by the LSP server (UPDATE / TOKENS / HINTS / CLOSE)

It produces the inlay hints and diagnostics that [arche.nvim](#editor-support-archenvim)
renders.

## Build

The build uses `make`:

| Target | What it does |
| ------ | ------------ |
| `make` / `make all` | build the compiler + all supporting tools |
| `make run` | compile and run `examples/stuff.arche` |
| `make test` | full suite: the lit fixtures **and** `make test-doc` |
| `make test-doc` | doctests over real sources (`arche test core/... examples/...`) |
| `make test-parser` / `make test-semantic` / `make test-codegen-unit` / `make test-lower` | C unit tests |
| `make format` | format all `.arche` and `.c`/`.h` files (pinned clang-format) |
| `make verify-syntax` | verify the syntax tree round-trips losslessly |
| `make verify-codegen` | diff emitted LLVM IR against golden files |
| `make clean` | remove build artifacts |

Tools built by `make all`: `build/arche` (compiler), `build/arche-fmt` (formatter),
`build/arche-analyzer` (editor analysis), `build/arche-syntax-tokens` (syntax-highlight token
dumper), plus the unit-test binaries.

## Documentation generation (planned)

An `arche doc` site generator (rustdoc-style) is **deferred** — `arche test` already runs the doc
examples (see [DOCTESTS.md](DOCTESTS.md)), but there is no rendered site yet.

**TODO — generated Vocabulary page.** When the renderer lands, it should compile a browsable
**Vocabulary** page straight from the program's schema (sourced from the same `SemModel` /
`DeclSummary` data the compiler already builds), *not* a hand-written reference. The page is the
data-model index a data-oriented program most wants:

- **Component types** — every global component name (one canonical type, [E0045](explain/E0045.md)),
  its backing type, and which archetypes carry it.
- **Archetypes** — each named shape = its component set (weightless; a shape always exists, naming it
  just gives the set a label).
- **Pools** — each `[N]Shape` allocation, with capacity and initial count — the only concrete global
  state.
- **Enums / distinct aliases** — the rest of the type vocabulary.

This makes the "fully global" schema (vocabulary that can't be scoped) legible without a dedicated
in-source marker: the page *is* the rendered global context.
