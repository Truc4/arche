# Arche patterns - do vs don't

Idioms and anti-idioms found while building real programs in Arche. Each entry is a concrete
"don't / do" with the reasoning, so the _shape_ of the mistake is recognizable next time.

Every ```arche block below is a real, complete program that the doctest runner compiles and runs
(`arche test docs/patterns.md`). Anti-examples — the "don't" snippets — are shown as plain text,
because deliberately-discouraged code is not something to present as exemplary arche.

## Visibility bands: prefer bare `#module` / `#file` over `{ }` blocks

A `#module` / `#file` marker is a **banner**, not a block: it narrows visibility for *the rest of
the file*. Everything below a `#module` banner is module-private (visible across the module's files,
invisible to importers); everything below `#file` is file-private. The default band at the top of
the file is the exported (public) surface.

**Don't** wrap private sections in braces:

```
#module {            // works, but reads like a scope and adds a layer of indentation
  TrieNode :: arche { ... }
}
#file {
  helper :: func() -> int { ... }
}
```

**Do** use bare banners and let them run to end of file, organizing public → private top-to-bottom:

```arche
// public API (default band = exported)
classify :: func(n: int) -> int { return n * 2; }

#module                       // everything below is module-private
internal_helper :: func() -> int { return classify(21); }
```

**Why:** the banner form is the intended idiom — one line, no extra indentation, and it matches the
"a band narrows the rest of the file" model. Reserve the `{ }` block form for the rare case where
you must return to a *wider* band afterward (you can't un-narrow a bare banner). Note that braces
work for `#foreign` extern groups too, but for `#module`/`#file` they are usually the wrong default.

## Declaration order: all top-level names hoist (order is free)

Every top-level name — procs, funcs, archetypes, enums, pools, **and** mutable global bindings
(`name : T[N]` buffers, `name : T` / `name := v` scalars) — is visible across the whole file
regardless of source order. A declaration may reference one that appears later.

**Do** put the public API first and the helpers it uses below (e.g. behind a `#module` / `#file`
banner) — forward references resolve regardless of source order:

```arche
entry :: func() -> int { return helper_a() + 1; }   // ok — helper_a is declared below

#module
helper_a :: func() -> int { return helper_b() * 2; }   // and helper_b is below this
helper_b :: func() -> int { return 20; }
```

```arche
fmt.assert(entry() == 41, "forward references should resolve\n");
```

**Why it matters:** this is what lets the visibility-band idiom (public API on top, internals behind
a trailing banner) coexist with the helpers a public proc uses. Mutable global bindings
(`name : T[N]` buffers, `name := v` scalars) hoist the same way. There is no "declare it first"
constraint at top level — only locals, inside a proc/func body, are point-of-introduction (they have
control flow and lifetime).

## Returning a variable number of results without a heap

There is no heap: a proc cannot allocate an array and return it, and returning a pointer into its
own frame would dangle. So the storage for a variable-length result must be **pre-reserved by the
caller or by the module** — a fixed, bounded slot either way.

**Don't** try to hand back a freshly-made array:

```
resolve :: proc(path: []char)(starts: []int, lens: []int, count: int)   // no storage exists for starts/lens
```

**Do** thread an **owned slice** (`own T[]`): the caller `move`s a bounded buffer in, the func
fills it and hands the fat pointer back, the caller rebinds it. The runtime length rides in the
slice, so no size appears in the signature, and there is still no heap — the storage is the caller's
buffer throughout:

```arche
fill_squares :: func(own xs: []int, n: int) -> []int {
  i := 0;
  for (; i < n; i += 1) {
    xs[i] = i * i;
  }
  return xs;
}
```

```arche
buf : [16]int;
out := fill_squares(move buf, 4);
fmt.assert(out[3] == 9, "fill_squares broken\n");   // .length flows back with the slice
```

Alternatives to the owned slice: (a) write into a caller-owned, bounded buffer passed by reference
(both in — "here's my storage" — and out — "now it's filled", the shadow of `io.fread`); or (b)
record into module-level state and expose accessors (what `router` does — `cap_start[8]` /
`cap_len[8]` / `cap_count[1]`, read back via `param(i)` / `param_count()`).

A func may return a slice only when it traces to a buffer passed *in* — returning a slice of a
fresh local would dangle and is rejected. (See language.md, "Arrays and slices".)

**Why:** "bounded" is the load-bearing word — a compile-time-known size is what lets the slot be a
fixed stack or static reservation instead of a heap allocation. The count is a plain `int` and
returns normally; only the *array* part needs the reservation. Caller-buffer (reentrant, per-call)
vs module-static (one shared slot, overwritten by the next call) is just *where* that fixed slot
lives. Spans (offset+length into the caller's own input) keep it zero-copy: nothing is owned or
duplicated.
