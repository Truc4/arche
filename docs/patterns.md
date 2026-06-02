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
resolve :: proc(path: char[])(handler_id: int) { ... }

#module                       // everything below is module-private
TrieNode :: arche { ... }

#file                         // everything below is file-private
helper :: func() -> int { ... }
```

**Why:** the banner form is the intended idiom — one line, no extra indentation, and it matches the
"a band narrows the rest of the file" model. Reserve the `{ }` block form for the rare case where
you must return to a *wider* band afterward (you can't un-narrow a bare banner). Note that braces
work for `#foreign` extern groups too, but for `#module`/`#file` they are usually the wrong default.

## Declaration order: globals are declare-before-use; procs/funcs/types are not

Procs, funcs, archetypes, enums, and archetype **pools** are *hoisted* — you may reference them
before their definition. Plain global **bindings** (`name: T[N]` buffers / scalars) are **not**:
they must be declared on an earlier source line than any code that reads them.

**Don't** put a public proc above the global state it touches:

```arche
resolve :: proc()(h: int) { h = cap_count[0]; }   // error: Undefined symbol 'cap_count'
cap_count: int[1]
```

**Do** lead with the global state, then the procs that use it:

```arche
cap_count: int[1]
resolve :: proc()(h: int) { h = cap_count[0]; }    // ok
```

**Why:** this interacts with the visibility-band idiom above. Because a bare banner forces the
exported API to the *top*, and a public proc's globals must precede it, those globals also live in
the leading (exported) band — even if you'd prefer them private. Hoisted decls (types, pools,
helper funcs/procs) have no such constraint, so push *those* below a `#module` / `#file` banner and
keep only the unavoidable leading globals up top. (See `stdlib/router/router.arche`: the `cap_*`
spans lead with the API; `TrieNode`/`nodeKind` and the parsing helpers trail behind the banners.)

## Returning a variable number of results without a heap

There is no heap: a proc cannot allocate an array and return it, and returning a pointer into its
own frame would dangle. So the storage for a variable-length result must be **pre-reserved by the
caller or by the module** — a fixed, bounded slot either way.

**Don't** try to hand back a freshly-made array:

```arche
resolve :: proc(path: char[])(starts: int[], lens: int[], count: int)   // no storage exists for starts/lens
```

**Do** either (a) write into a caller-owned, bounded buffer passed by reference (the buffer is both
in — "here's my storage" — and out — "now it's filled", the same shadow as `io.fread`):

```arche
resolve :: proc(path: char[], starts: int[], lens: int[])(starts: int[], lens: int[], count: int)
// caller: starts: int[8]; lens: int[8]; resolve(path, starts, lens)(starts, lens, count:)
```

or (b) record into module-level state and expose accessors (what `router` does — `cap_start[8]` /
`cap_len[8]` / `cap_count[1]`, read back via `param(i)` / `param_count()`).

**Why:** "bounded" is the load-bearing word — a compile-time-known size is what lets the slot be a
fixed stack or static reservation instead of a heap allocation. The count is a plain `int` and
returns normally; only the *array* part needs the reservation. Caller-buffer (reentrant, per-call)
vs module-static (one shared slot, overwritten by the next call) is just *where* that fixed slot
lives. Spans (offset+length into the caller's own input) keep it zero-copy: nothing is owned or
duplicated.
