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
pool and does the next step. A consumer `map (Q) eff` fan over an _empty_ pool runs zero times — the absence of
the row _is_ the skipped branch.

**Don't** thread the whole thing through one procedural pass, guarding each effect on the last result:

```
handle :: system eff {
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

seed :: system eff {
  insert(Request { code: 2 })(_:, _:);
  insert(Request { code: -1 })(_:, _:); // malformed
  insert(Request { code: 0 })(_:, _:);
}

// PARSE = the producer: route each request into the pool for its case. The `if (!ok) respond(400)` guard
// becomes "insert into Bad"; everything else goes to Routed. No `return`, no downstream gating.
parse :: map (query { code }) eff {
  if (code < 0) {
    insert(Bad { status: 400 })(_:, _:);
  } else {
    insert(Routed { handler: code })(_:, _:);
  }
}

// Two consumers, each over its OWN pool. `as r` binds the matched row's handle; after doing the n+1 step,
// `delete(r)` CONSUMES it — removes it from the pool. Neither asks "was there an error?": an empty pool
// runs zero times, and because each row is deleted, a later pass drains nothing (no re-processing).
errors :: map (query { status } as r) eff {
  fmt.printf("respond %d\n", status); // the error response …
  delete(r)(_:);                      // … then consume the Bad row (it's handled — remove it)
}
dispatch :: map (query { handler } as r) eff {
  fmt.printf("serve route %d\n", handler); // dispatch …
  delete(r)(_:);                           // … then consume the Routed row
}

#run seq({ seed, parse, errors, dispatch })
```

**Why:** a consumer `map (Q) eff` fan over an empty pool is a no-op — that _is_ the conditional. System 1
produces `n` (inserts a row); system 2 consumes `n` (does the n+1 step, then **`delete`s the row** via its
`as r` handle); the schedule orders writer-before-reader within the pass. "Branch on a result" becomes
"route the row into the pool for that case," and an absent row is simply never read — no per-step guards,
no intra-row effect→effect chaining. **Consuming = deleting**: a handled row leaves the pool, so it is
never re-processed on a later pass (a drained pool re-runs to nothing). This is the shape of arche-rpg's
`done :: map (Closed) eff`: it fires only once a `Closed` row exists, so "should we exit?" needs no boolean —
the row's presence is the signal.
