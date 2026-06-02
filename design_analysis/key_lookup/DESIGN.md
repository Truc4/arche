# Design decision: dense-integer keys by default; intern strings at the boundary

Just as data is **columnar (SoA) by default**, identity should be a **dense integer by default**.
A thing's key is a small dense int — a row index, a slot, a handle — and "look it up" is an array
read. String keys are the *exception*: when the outside world hands you strings, you **intern**
them to dense ids once at the edge, and use the int everywhere inside.

This is grounded in the lookup benchmark (`key_lookup/bench.c`, full table in the top-level
`README.md`), ns per lookup:

| strategy | N=16 | N=1000 | what it is |
|----------|-----:|-------:|------------|
| DENSE (id in hand)    | 0.37 | 0.35 | `value[id]` — key is already a dense int |
| HASH (string key)     | 14.0 | 21.1 | FNV-1a + probe + `strcmp` |
| INTERN (string→id→arr)| 14.6 | 20.9 | hash the incoming string, then `value[id]` |
| SCAN (`strcmp`)       | 27.7 | 1480  | linear compare every entry |

**Dense array indexing is ~40–60× a hash, and flat in N.** That gap is the whole argument.

## Why dense-integer keys by default

- **Lookup is a single array read** — no hash, no probe, no compare. ~0.35 ns, and it does not grow
  with the table size. It's the same SoA win applied to identity: the id *is* the offset into the
  column.
- **It composes with the columnar layout.** If the key is the row index, then every "look up this
  entity's fields" is just `Column[id]` across all its components — the access pattern systems
  already use. No separate index structure to keep coherent with the data.
- **No index to build, store, or invalidate.** A hash index is a second structure that must be
  maintained alongside the table; dense ids need none.
- **Predictable latency.** No load-factor cliffs, no probe-length variance, no rehash. The cost is
  one indexed load, every time.

## When you need string keys: intern at the boundary

Real input arrives as strings (a URL path, a header name, a symbol). The rule is **hash it to a
dense id exactly once, at ingest** — then carry the int through storage, joins, comparisons, and
repeated lookups. The hash happens at the edge, not on every internal access.

- The intern table is itself a string hash index (`HASH` above): `string → id`, built once.
- Everything downstream of the intern point is back on the `DENSE` fast path.
- The anti-pattern this avoids: **re-hashing the same string over and over** as it flows through the
  system. Hash once, then it's an int.

## When NOT to intern

Interning only pays off **if the id is reused.** The benchmark is blunt about this: `INTERN ≈ HASH`.
If a string key is looked up exactly once with no downstream reuse — a one-shot request path matched
and discarded — interning buys nothing, because you pay the boundary hash either way. In that case
just hash (or, for a handful of candidates, `SCAN` is competitive: at tiny N a linear `strcmp` is
about one hash). Reach for interning when the same string becomes an identity that travels.

## Consequence for structures (e.g. the URL router)

- Don't reach for "hash keys on a pool" as a default — a pool's identity is *already* its dense slot
  (`i32(insert(...))`). That's the dense key; use it directly.
- A radix router resolves a string path by walking dense-id child links; each node's child match is
  a tiny scan (≈ one hash per segment), and the only place a string→id hash is justified is if a
  path segment's id is reused downstream. For one-shot resolution, no interning, no hash index.
- A flat hash index earns its place only when you genuinely have a large set of *string* keys with
  no dense-int identity available and repeated lookups — the narrow case where `HASH`'s flat
  ~15–21 ns beats both `SCAN` (O(N)) and the cost of inventing dense ids.

**Default to dense. Intern strings at the boundary when the id will be reused. Otherwise just
hash.**
