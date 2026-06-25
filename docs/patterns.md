# Arche patterns - do vs don't

Idioms and anti-idioms found while building real programs in Arche. Each entry is a concrete
"don't / do" with the reasoning, so the _shape_ of the mistake is recognizable next time.

## Visibility bands: prefer bare `#module` / `#file` over `{ }` blocks

A `#module` / `#file` marker is a **banner**, not a block: it narrows visibility for _the rest of
the file_. Everything below a `#module` banner is module-private (visible across the module's files,
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
you must return to a _wider_ band afterward (you can't un-narrow a bare banner). Note that braces
work for `#foreign` extern groups too, but for `#module`/`#file` they are usually the wrong default.

## Producer / consumer: route data into a pool instead of branching on it

A step that _might_ produce a result shouldn't gate everything after it with "if I got a result, do
the next thing." Have the producer **write a row into a pool** (or not); a later system reads that
pool and does the next step. A consumer `each` over an _empty_ pool runs zero times — the absence of
the row _is_ the skipped branch.

**Don't** thread the whole thing through one procedural pass, guarding each effect on the last result:

```
handle :: system {
  parse(req)(method:, target:, ok:);
  if (!ok) { respond(conn, 400); return; }       // guard
  resolve(target)(id:);
  if (id < 0) { respond(conn, 404); return; }     // guard
  serve(conn, target);                            // …and so on, every step gated by the previous
}
```

**Do** let a producer route rows into a pool, and a consumer process whatever is there — with no
"is there anything?" check:

```arche
#import { fmt }

// Incoming requests. `code` >= 0 is a well-formed request carrying a handler id; < 0 is malformed.
Request :: arche { code :: int }
Bad     :: arche { status :: int }  // malformed requests route here → an error response
Routed  :: arche { handler :: int } // well-formed requests route here → dispatched
[4]Request;
[4]Bad;
[4]Routed;

seed :: system {
  insert(Request { code: 2 })(_:, _:);
  insert(Request { code: -1 })(_:, _:); // malformed
  insert(Request { code: 0 })(_:, _:);
}

// PARSE = the producer: route each request into the pool for its case. The `if (!ok) respond(400)` guard
// becomes "insert into Bad"; everything else goes to Routed. No `return`, no downstream gating.
parse :: each (query { code }) {
  if (code < 0) {
    insert(Bad { status: 400 })(_:, _:);
  } else {
    insert(Routed { handler: code })(_:, _:);
  }
}

// Two consumers, each over its OWN pool, each doing the actual n+1 step on the row it consumes — `errors`
// sends the error response, `dispatch` serves the handler. Neither asks "was there an error?": an empty
// pool runs zero times, so the 400 path simply doesn't fire when every request parsed.
errors :: each (query { status }) {
  fmt.printf("respond %d\n", status); // consume the Bad row → write the error response
}
dispatch :: each (query { handler }) {
  fmt.printf("serve route %d\n", handler); // consume the Routed row → dispatch to its handler
}

#run seq({ seed, parse, errors, dispatch })
```

**Why:** a consumer `each` over an empty pool is a no-op — that _is_ the conditional. System 1
produces `n` (writes a row/column); system 2 consumes `n` and does the n+1 step; the schedule orders
writer-before-reader within the pass. "Branch on a result" becomes "route the row into the pool for
that case," and an absent row is simply never read — no per-step guards, no intra-row effect→effect
chaining. This is the shape of arche-rpg's `done :: each (Closed)`: it fires only once a `Closed` row
exists, so "should we exit?" needs no boolean — the row's presence is the signal.
