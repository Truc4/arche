# Arche patterns — bad vs good

Idioms and anti-idioms found while building real programs in Arche. Each entry is a concrete
"don't / do" with the reasoning, so the *shape* of the mistake is recognizable next time.

## Contents

- [Reporting variable output: spans, not owned buffers](#reporting-variable-output-spans-not-owned-buffers)

---

## Reporting variable output: spans, not owned buffers

**Where it bit:** the radix URL router (`stdlib/router/router.arche`). `route_resolve` matches a
path like `/users/42` and must hand back captured `:param` / `*catchall` segments.

### ✗ Bad — the routine owns caller buffers

```arche
// route_resolve fills two caller-lent buffers, picking one by capture index.
route_resolve :: proc(path: char[], p0: char[], p1: char[])(p0: char[], p1: char[], handler_id: int) {
  // ... on a :param at path[s..e) ...
  if (pidx == 0) {                 // ← which named buffer? branch on it.
    for (i := s; i < e; i = i + 1) { p0[j] = path[i]; j = j + 1; }
    p0[j] = 0;
  }
  if (pidx == 1) {                 // ← duplicated for p1
    for (i := s; i < e; i = i + 1) { p1[j] = path[i]; j = j + 1; }
    p1[j] = 0;
  }
  pidx = pidx + 1;
}
```

Two things rot here, and they reinforce each other:

1. **The in-out *shadow* (a name in both the in-list and out-list) is an FFI mechanism**, not a
   general "return a string" tool. It exists so an Arche `(in)(out)` shape can line up with a C
   function's argument order + return value (see [language.md](language.md) "Foreign resources").
   Reaching for it in pure-Arche code drags in ownership friction: try to factor the copy into a
   helper and you immediately fight the borrow checker —
   `cannot move read-only parameter 'p0' — it is borrowed, not owned`.
2. **The outputs are separate *named* buffers**, so selecting one means branching on the index
   (`if (pidx == 0) … if (pidx == 1) …`). That `if`-nesting is not the disease — it's the symptom.
   You cannot index `p0`/`p1` by a runtime number, so the control flow has to fan out by hand, and
   the body duplicates per slot. Adding a third param means a third branch.

The root cause is **the routine trying to own the output.** Once a producer commits to writing
into caller buffers it doesn't own, every downstream choice is bad.

### ✓ Good — report *spans*, let the caller slice

The caller already holds the `path`. Don't copy out of it — just say *where* each match is
(offset + length). Capture state is module data, indexed by a count, so there is no per-buffer
branching at all:

```arche
static cap_start: int[8]   // span offsets into the resolved path
static cap_len:   int[8]
static cap_count: int[1]

route_resolve :: proc(path: char[])(handler_id: int) {
  cap_count[0] = 0;
  // ... on a :param / *catchall at path[s..e) — no buffers, no branch on which slot:
  cap_start[cap_count[0]] = s;
  cap_len[cap_count[0]]   = e - s;
  cap_count[0] = cap_count[0] + 1;
}

// Accessors (procs, because a `func` may not read a `static`):
route_param_count :: proc()(n: int)            { n = cap_count[0]; }
route_param       :: proc(i: int)(start: int, len: int) { start = cap_start[i]; len = cap_len[i]; }
```

```arche
// Caller — zero copy. Slice the path you already own; materialize only if you actually need a
// C-string (e.g. for atoi), and that copy is then *your* choice, not forced by the API.
route_resolve(req_path)(h:);
route_param(0)(st:, ln:);
// id lives at req_path[st .. st+ln)
```

**Why it's better:** no output buffers in the signature, no ownership transfer, no copy on the hot
path, and — because spans live in an `int[]` indexed by `cap_count` — **the index branching is
gone**: one straight-line write per capture, N captures with no code change. The integer arrays
*are* indexable; the named `char[]` buffers were not. (For one fixed buffer that the proc fills
once, in-out is still fine — e.g. `read_chunk(fd, buf, n)(buf, n:)`. The anti-pattern is using it
to return a *variable* number of results.)

**Alternative — invert control (IOC).** If the caller shouldn't even see offsets, pass a
monomorphized callback the router invokes once per capture (`sink(idx, path, start, len)`). Same
win: the producer owns nothing and never branches on a slot. Spans win when the caller wants data;
a callback wins when the caller wants behavior.

**Rule of thumb:** a routine that produces a *variable* amount of output should report **positions
in the caller's data**, or **call back into the caller** — it should not own and fill the caller's
buffers. If you find yourself branching on *which output buffer*, the data shape is wrong, not the
control flow.
