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
arche [flags] input.arche          # compile to an executable
arche test <path> [-v]             # run doctests (see DOCTESTS.md)
arche init device <name>           # scaffold a device folder (shape + system + doctest)
arche init driver <name>           # scaffold a driver file (imports, sizes a pool, runs a system)
arche --explain <code>             # print the long-form explanation for a diagnostic
```

Compile flags:

- `-o <file>` - output executable path
- `-emit-llvm -o <file.ll>` - emit LLVM IR instead of an executable
- `--link <path>` - pass an extra `.c`/`.o` to `cc` at link time (repeatable)
- `-Wno-proc-could-be-func`, `-Wno-proc-no-effect` - disable lints
- `-Werror=proc-could-be-func`, `-Werror=proc-no-effect`, `-Werror` - promote lints to errors

`arche test` runs the runnable examples in `///` doc comments - see [DOCTESTS.md](DOCTESTS.md).
`arche --explain E0001` reads the long-form note from `docs/explain/<code>.md`.
`arche init` scaffolds the device/driver templates - see [devices.md](devices.md). It never
overwrites an existing file.

## Diagnostics

Errors carry stable codes `E0001`–`E0203`; warnings `W0001`–`W0010` (promotable to
`-Werror`). Codes are stable forever (burn-on-delete). A sample:

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
| `make verify-cst` | verify the CST round-trips losslessly |
| `make verify-codegen` | diff emitted LLVM IR against golden files |
| `make clean` | remove build artifacts |

Tools built by `make all`: `build/arche` (compiler), `build/arche-fmt` (formatter),
`build/arche-analyzer` (editor analysis), `build/arche-cst-tokens` (syntax-highlight token
dumper), plus the unit-test binaries.
