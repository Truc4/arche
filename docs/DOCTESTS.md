# Documentation comments & doctests

arche supports Rust/Python-style documentation comments whose **examples are run
as tests**, so the code in your docs can never silently rot.

## Doc comments

- `/// ...` — an **outer** doc comment. Attaches to the declaration immediately
  below it (a `func`, `proc`, `sys`, archetype, `const`, …). A blank line or a
  plain `//` comment between the doc and the declaration detaches it.
- `//!` — an **inner**/module-level doc comment (file-level docs).
- `//// ...` (four or more slashes) is a plain banner comment, not a doc comment.

Doc text is Markdown. Inside it, a fenced block tagged `arche` is a runnable
example:

````arche
/// Adds two integers.
///
/// # Example
/// ```arche
/// assert(add(2, 3) == 5, "add broken\n");
/// ```
func add(a: int, b: int) -> int {
  return a + b;
}
````

## Running doctests

```
arche test <file.arche>     # one file
arche test <dir>            # every .arche under <dir>, recursively
arche test ./...            # Go-style: recurse from the current directory
```

Recursive runs skip files with no examples and print a grand total
(`N passed, M failed, K ignored across F files`); they exit non-zero if any
example failed. `build/`, hidden, `node_modules`, and `site-packages` dirs are
skipped.

For each ```arche example the runner:

1. brings the file's API into scope (it compiles the example with `use <file>;`),
2. wraps the example in a `proc main` if it doesn't declare one,
3. compiles and runs it.

An example **passes if it compiles and exits 0**. There is nothing special to
remember: an example is ordinary arche code that must run cleanly. To check a
value, use the ordinary `assert(condition, message)` from the core library —
the same call you'd write in any test. A failed `assert` exits non-zero, so the
doctest fails.

The command exits non-zero if any example fails, so it drops straight into CI
(`make test` runs every `tests/unit/doctest/*.arche`). Each example runs under a
10-second timeout; a hanging example is killed and reported `(timed out)`. A
failing example reports the exact source line of its ```arche fence.

### Fence flags

The fence info string takes Rust-style flags after `arche`:

- `` ```arche `` — compile and run; pass on exit 0 (the default).
- `` ```arche,no_run `` — compile only, don't run.
- `` ```arche,compile_fail `` — must *fail* to compile (documents a misuse).
- `` ```arche,should_panic `` — must exit non-zero when run.
- `` ```arche,ignore `` — shown in docs but never compiled or run.

### What goes in a doctest

- **Example/API-shaped tests** belong inline in doc comments — they double as
  documentation.
- **Regression / negative / edge-case tests** belong in normal test files; they
  aren't documentation and would clutter the docs.

### Limitation: documented files are libraries

Because the runner does `use <file>;`, the documented file must not define a
top-level `proc main` of its own — that would collide with the example's `main`
(a duplicate-declaration compile error, reported as a failing doctest). This
matches Rust, where doctest targets are library crates. Document libraries, not
programs.

## Editor hover

`arche-analyzer` surfaces doc comments for editor hover. In `--dump` mode it
emits, per documented declaration:

```
DOC <line> <col> <name> <linecount>
DOCLINE <text>            × linecount
```

The language server renders the `DOCLINE`s as the hover body.
