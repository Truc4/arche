# Documentation comments & doctests

arche supports Rust/Python-style documentation comments whose **examples are run
as tests**, so the code in your docs can never silently rot.

## Doc comments

- `/// ...` - an **outer** doc comment. Attaches to the declaration immediately
  below it (a `func`, `proc`, `sys`, archetype, `const`, …). A blank line or a
  plain `//` comment between the doc and the declaration detaches it.
- `//!` - an **inner**/module-level doc comment (file-level docs).
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
arche test -v ./...         # verbose: a line per example, not just per file
```

Output is `go test`-style: one status line per file, details only on failure.

```
$ arche test ./...
ok   ./math.arche
    --- FAIL: neg (line 2): exit 1
        neg is wrong!                 ← the failing example's own output
FAIL ./sub/broken.arche
```

`ok` = all examples passed, `FAIL` = at least one failed, `?` = no examples.
Files with no examples are silent in a tree sweep. The process exits non-zero if
any example failed. `build/`, hidden, `node_modules`, and `site-packages` dirs
are skipped.

For each ```arche example the runner:

1. brings the file's API into scope (it compiles the example with `use <file>;`),
2. wraps the example in a `proc main` if it doesn't declare one,
3. compiles and runs it.

An example **passes if it compiles and exits 0**. There is nothing special to
remember: an example is ordinary arche code that must run cleanly. To check a
value, use the ordinary `assert(condition, message)` from the core library -
the same call you'd write in any test. A failed `assert` exits non-zero, so the
doctest fails.

The command exits non-zero if any example fails, so it drops straight into CI.
`make test` runs the lit suite **and** `make test-doc`, which runs
`arche test core/... examples/...` over the real sources (`arche test` accepts
multiple path specs). Each example runs under a 10-second timeout; a hanging
example is killed and reported `(timed out)`. A failing example reports the exact
source line of its ```arche fence.

> `core/core.arche` is auto-prepended to every program, so doctests *in core*
> must not `use core;` - the runner detects this and omits it. `strlen`/`atoi`
> in core carry tested examples as a reference.

### Fence flags

The fence info string takes Rust-style flags after `arche`:

- `` ```arche `` - compile and run; pass on exit 0 (the default).
- `` ```arche,no_run `` - compile only, don't run.
- `` ```arche,compile_fail `` - must *fail* to compile (documents a misuse).
- `` ```arche,should_panic `` - must exit non-zero when run.
- `` ```arche,ignore `` - shown in docs but never compiled or run.

### What goes in a doctest

- **Example/API-shaped tests** belong inline in doc comments - they double as
  documentation.
- **Regression / negative / edge-case tests** belong in normal test files; they
  aren't documentation and would clutter the docs.

### Limitation: documented files are libraries

Because the runner does `use <file>;`, the documented file must not define a
top-level `proc main` of its own - that would collide with the example's `main`
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

## Markdown doctests

`arche test` also runs ```arche fenced blocks inside **`.md` files** — so prose docs
(`docs/*.md`, a project's README) are compiled and run, not just inspected. A directory sweep
(`arche test docs/...`) picks up `.md` and `.arche` files alike.

Unlike `.arche` doctests (which run with their documenting file's full context), a `.md` block has
no documenting file. Instead, blocks are grouped by **markdown section**:

- Every ```arche block under the same heading shares one scope — the section's top-level
  declarations (`name :: …` forms, `#module`/`#file`/`#foreign` regions) and imports are pooled,
  and each block's loose statements run in their own `main` with that scope visible. So you declare
  a type once and reuse it in a later block of the same section, with no repeated setup.
- A **new heading starts a fresh scope.** Two sections are isolated, so independent examples may
  reuse names (and each may have its own `#module`) without colliding.

`fmt` is always available (no import needed). A section that contains only declarations is
compile-checked; a section with statements runs them.

### Convention: every ```arche block is runnable

Markdown doctests have **no opt-out flags** — every ```arche block must be a real, complete program
that compiles and exits 0. There is deliberately no `ignore`/`no_run`/`compile_fail` here: a doc
example you wouldn't run is a doc example that can rot.

A deliberately-wrong "don't write this" snippet is therefore **not** tagged `arche` — write it as a
plain ` ``` ` (or ` ```text `) fence so the runner skips it. It isn't valid arche; it shouldn't be
presented as exemplary arche.
