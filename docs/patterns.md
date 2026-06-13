# Arche patterns - do vs don't

Idioms and anti-idioms found while building real programs in Arche. Each entry is a concrete
"don't / do" with the reasoning, so the _shape_ of the mistake is recognizable next time.

## Visibility bands: prefer bare `#module` / `#file` over `{ }` blocks

A `#module` / `#file` marker is a **banner**, not a block: it narrows visibility for *the rest of
the file*. Everything below a `#module` banner is module-private (visible across the module's files,
invisible to importers); everything below `#file` is file-private. The default band at the top of
the file is the exported (public) surface.

**Don't** wrap private sections in braces:

```arche
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
resolve :: proc(path: []char)(handler_id: int) { ... }

#module                       // everything below is module-private
TrieNode :: arche { ... }

#file                         // everything below is file-private
helper :: func() -> int { ... }
```

**Why:** the banner form is the intended idiom — one line, no extra indentation, and it matches the
"a band narrows the rest of the file" model. Reserve the `{ }` block form for the rare case where
you must return to a *wider* band afterward (you can't un-narrow a bare banner). Note that braces
work for `#foreign` extern groups too, but for `#module`/`#file` they are usually the wrong default.

## Declaration order: all top-level names hoist (order is free)

Every top-level name — procs, funcs, archetypes, enums, pools, **and** mutable global bindings
(`name : T[N]` buffers, `name : T` / `name := v` scalars) — is visible across the whole file
regardless of source order. A declaration may reference one that appears later.

**Do** put the public API first and the state/helpers it uses below (e.g. behind a `#module` /
`#file` banner) — forward references resolve:

```arche
resolve :: proc()(h: int) { h = cap_count[0]; }   // ok — cap_count is declared below

#module
cap_count : [1]int
```

**Why it matters:** this is what lets the visibility-band idiom (public API on top, internals behind
a trailing banner) coexist with private state a public proc uses. There is no "declare the global
first" constraint — locals are still point-of-introduction, but top level is order-free. (Locals,
inside a proc/func body, must still be declared before use — they have control flow and lifetime.)

## Returning a variable number of results without a heap

There is no heap: a proc cannot allocate an array and return it, and returning a pointer into its
own frame would dangle. So the storage for a variable-length result must be **pre-reserved by the
caller or by the module** — a fixed, bounded slot either way.

**Don't** try to hand back a freshly-made array:

```arche
resolve :: proc(path: []char)(starts: []int, lens: []int, count: int)   // no storage exists for starts/lens
```

**Do** either (a) write into a caller-owned, bounded buffer passed by reference (the buffer is both
in — "here's my storage" — and out — "now it's filled", the same shadow as `io.fread`):

```arche
resolve :: proc(path: []char, starts: []int, lens: []int)(starts: []int, lens: []int, count: int)
// caller: starts: [8]int; lens: [8]int; resolve(path, starts, lens)(starts, lens, count:)
```

or (b) record into module-level state and expose accessors (what `router` does — `cap_start[8]` /
`cap_len[8]` / `cap_count[1]`, read back via `param(i)` / `param_count()`).

or (c) thread an **owned slice** (`own T[]`): the caller `move`s a bounded buffer in, the func
fills it and hands the fat pointer back, the caller rebinds it. The runtime length rides in the
slice, so no size appears in the signature, and there is still no heap — the storage is the caller's
buffer throughout:

```arche
fill :: func(own xs: []int, n: int) -> []int { ... xs[i] = …; return xs; }
// caller: buf: [16]int; buf := fill(move buf, 16);   // .length flows back with it
```

A func may return a slice only when it traces to a buffer passed *in* — returning a slice of a
fresh local would dangle and is rejected. (See language.md, "Arrays and slices".)

**Why:** "bounded" is the load-bearing word — a compile-time-known size is what lets the slot be a
fixed stack or static reservation instead of a heap allocation. The count is a plain `int` and
returns normally; only the *array* part needs the reservation. Caller-buffer (reentrant, per-call)
vs module-static (one shared slot, overwritten by the next call) is just *where* that fixed slot
lives. Spans (offset+length into the caller's own input) keep it zero-copy: nothing is owned or
duplicated.
