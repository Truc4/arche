# Callbacks → data (the proc-callback replacement)

`proc` is removed (except `#foreign`/`@syscall`/`@intrinsic`/`@drop`), and the `Eff` value layer is a free
*selective* — **no value-level `bind`** — and there are **no higher-order systems** (flat-effect model §5).
A "callback" (a proc-typed param) is two different things; neither needs procs.

## Pattern 1 — continuation callback ("do X, then call me back") → producer/consumer + schedule

This is the monadic pattern (`run task >>= \result -> on_done result`) worn as a callback. arche replaces
it with a **data edge**: the producer emits a row; the "callback" is a consumer system that runs over that
pool. The schedule's order *is* the "then".

**Before** (higher-order proc — invokes a passed-in proc):

```arche
done_handler :: proc()();                       // a callback TYPE

finish :: proc()() {                            // the callback impl
  fmt.print("task done\n")(_:);
}

run_task :: proc(work: int, on_done: done_handler)() {
  // …do the work…
  on_done()();                                  // call the callback when finished
}

main :: proc() {
  run_task(42, finish)();
}
```

**After** (the "callback" is a consumer system over an emitted pool):

```arche
Work :: arche { n :: int }   [4]Work;
Done :: arche { code :: int } [4]Done;          // "task finished" — as data

seed :: system { insert(Work { n: 42 })(_:, _:); }

run_task :: each (query { n } as w) {           // process each work request
  // …do the work…
  insert(Done { code: n })(_:, _:);             // EMIT a Done row instead of calling on_done
  delete(w)(_:);                                // consume the request
}

finish :: each (query { code } as d) {          // the "on_done" callback = a consumer system
  fmt.printf("task done: %d\n", code);          // runs zero times if no Done row was emitted
  delete(d)(_:);
}

#run seq({ seed, run_task, finish })            // the `seq` ordering IS the "then"
```

The continuation became *the next system in the schedule*; the argument became *a row in a pool*.
Branch-on-result → route into different pools; "call back N times" → N rows + one fanned consumer.

## Pattern 2 — pure strategy callback ("inject behavior") → a `func` + the existing HOF surface

A comparator, mapper, or folding op is a *pure* function — it was never an effect. It becomes a `func`,
passed to the higher-order surface arche already has (`reduce`'s operator, `|>`'s finalizer) or just called.

**Before** (a proc used only to compute a value, passed as a strategy):

```arche
unary :: proc(x: int)(r: int);                  // a "callback type"

double :: proc(x: int)(r: int) { r = x * 2; }

apply :: proc(v: int, op: unary)(r: int) {      // higher-order proc
  op(v)(r);
}

main :: proc() {
  apply(5, double)(out:);
}
```

**After** (pure `func`; no callback indirection — call it, or hand it to a real HOF):

```arche
double :: func(x: int) -> int { return x * 2; }

entry :: system {
  r := double(5);                  // direct — the indirection bought nothing
  fmt.printf("r=%d\n", r);
}

// the pure-strategy HOF surface arche already supports:
//   reduce(+, Particle.mass)       // `+` is the injected operator
//   clock_mono() |> ms_of          // `ms_of` is the injected pure finalizer
```

A *pure* strategy can be a first-class `func` value without reintroducing higher-order **effects** — that
is a separate, safe question from the proc-callbacks removed here.

## Summary

| callback kind | what it really is | arche replacement |
|---|---|---|
| continuation ("then call back") | a hand-rolled monad (`bind`) | **producer/consumer + schedule** — emit a row, a consumer system reacts |
| pure strategy ("inject behavior") | a higher-order *pure* function | a `func`, passed to `reduce`/`|>` or called directly |

Proc-typed callbacks are exactly the monadic / higher-order-effect pattern the flat model rejects, so they
do not survive proc-elimination. (The current `lint_proc_decl` exemption for proc-callback params is a
"not yet" TODO, not an endorsement.)
