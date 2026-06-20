# Migrating an ETL pipeline to the flat effect model

### a concrete before/after for [the-flat-effect-model](the-flat-effect-model.md)

> **Status.** The model is a *design* (see [the-flat-effect-model.md](the-flat-effect-model.md)); the
> `Eff`/`each`/`#schedule` forms here are not implemented. The **"before"** is real current source from
> `design_analysis/benchmarks/etl/`. The **"after"** is illustrative — what the same pipeline looks like
> under the model. This is the demo where the model's *engine* actually turns over: the output is a
> **column of effects**, fanned in one kernel — the thing a one-at-a-time server never has.

---

## The program in one breath

Load a CSV of transactions, derive a column (`revenue = price × quantity`), write the result back out.
Genuinely columnar: 1k rows in the small task, **100M** in the large one. Three stages — read, a pure
column transform, write — over a wide pool.

## Before — `design_analysis/benchmarks/etl/1k/single-thread/arche/task_1_derived_columns.arche`

```arche
Transaction[1000];                               // pool: 1000 rows
Transaction :: arche { price :: float   quantity :: int   revenue :: float }

main :: proc() {
  csv.load(Transaction, "design_analysis/benchmarks/etl/data/data_1k.csv");   // I/O: read + parse
  Transaction.revenue = Transaction.price * Transaction.quantity;             // pure column op
  write_transactions();
}

write_transactions :: proc() {
  io.fopen_write("build/benchmarks/etl/task1/arche_output.csv")(fd:);         // I/O: open
  for (i := 0; i < 1000; i = i + 1) {
    buf: [256]char;
    fmt.sprintf(_, "%f,%d,%f\n",
      Transaction.price[i], Transaction.quantity[i], Transaction.revenue[i])(buf, n:);  // pure: format
    io.fwrite(fd, move buf, n);                                               // I/O: write one row
  }
  io.fclose(move fd);                                                         // I/O: close
}
```

The shape to notice: the transform (`revenue = price * quantity`) is *already* a pure, data-parallel
column op — arche is DOD, so the "math" stage needs no migration; it is already in the model's native
form. The interesting part is the **write**: a `for` loop running one `io.fwrite` per row.

## After — the write becomes a column of effects, fanned in one kernel

Split the write into a **pure** per-row formatter (a `map` kernel building a `line` column) and an
**impure** per-row leaf (an `each` kernel running the write and capturing per-row failure as data):

```arche
Transaction :: arche { price :: float   quantity :: int   revenue :: float   line :: []char   ok :: bool }

derive :: map query { price, quantity, revenue } { revenue = price * quantity; }    // pure: the math
format :: map query { price, quantity, revenue, line } {                            // pure: build the bytes
  line = f32(price) <> "," <> int(quantity) <> "," <> f32(revenue) <> "\n";
}
flush  :: each query { line, ok } (fd: int) {                                       // effect leaf, fanned
  io.fwrite(fd, line)(res:);                                                        // run the write per row
  ok = res is Ok;                                                                   // per-row failure as a column
}

write_out :: system {                  // composer: open, fan the writes, close — threads fd
  io.fopen_write("…/out.csv")(opened:);
  fd := opened ? Ok => f | Err => { return; };
  run format;                          // pure pass: fill the `line` column
  run flush(fd);                       // impure fan: run N writes as one kernel
  io.fclose(fd);
}

#schedule { load; derive; write_out; }     // the tick body
```

ETL is the degenerate pacing case: **a single tick**. There's no loop — the driver just calls `tick()` once
(or `for n { tick() }` to re-run the pipeline over fresh input). No clock, no `loop` in the schedule; the
ordered stages *are* the one tick. (Contrast the rpg/server demos, whose drivers pace many ticks.)

## Where the model earns its keep here (the strengths)

1. **The write is a real effect column.** `line` is built in a pure `map` (no I/O), then drained by one
   fanned `each` — N writes expressed as a single kernel instead of a hand-rolled loop. This is the
   pattern §6 of the model is built around, and unlike the web server, **there is genuine fan-out** (1k–100M
   rows) for it to act on. The deferred-effect / drain-at-a-barrier machinery has something to do.
2. **Per-row failure is data, not an abort.** The `ok` column records which rows failed to write. The
   original `for` loop *discards* `io.fwrite`'s result entirely — a silent partial-write bug. The model
   makes failure a column you can filter, count, or re-drive.
3. **The transform is a pure, testable, offloadable kernel.** `derive`/`format` are pure column ops:
   assert on the `revenue` column directly, no I/O; and being kernels, they are exactly what the
   data-parallel/GPU backend can run. The pure/impure seam is crisp — `load` (I/O) → `derive`/`format`
   (pure) → `flush` (I/O).

## Where it doesn't pay (the weaknesses — honest)

1. **For *this* 3-column task, the current code is already fine.** One multiply and a straight write loop
   are clear as-is; `map`/`each`/`Eff`/`system` is more vocabulary for little gain at this size. The win
   scales with the number of transforms and with per-row *conditional* handling (skip/route bad rows) —
   neither of which this task has. The model's leverage is real but latent here.
2. **`csv.load` doesn't improve — it can't.** Parsing decides structure from runtime bytes (where's the
   next comma, this row's field count) — that's result-dependent, i.e. monadic. It can't be a pure func
   or an `Eff`; it stays an opaque primitive proc. The load stage is exactly the un-decomposable residue
   the web server hit with its read-loop. The model leaves loading untouched.
3. **`fmt.sprintf` is the hard descriptor (§8).** Heterogeneous varargs (`"%f,%d,%f"`) strain "an effect
   is a bounded value." The `<>`-builder form above sidesteps it, but format-string-shaped effects remain
   the awkward corner of the value model.
4. **The reduction isn't part of the effect story.** Summing `revenue` (the benchmark's checksum) is a
   pure fold — the `reduce` collective — not an effect. Fine, but it means the "effect" content of an ETL
   job is just the I/O at the two ends; the middle is all pure data-parallel work that never needed the
   effect model to begin with.

## Net

ETL is where the effect column actually runs: the write fans across rows in one kernel, failure becomes a
column, and the transform is a pure offloadable kernel — the engine the web server left idle. But this
specific task is small and linear enough that the current straight-line code is already good; the model's
payoff grows with transform count and per-row branching, and it does nothing for the one genuinely hard
part (parsing on load).
