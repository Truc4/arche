```arche
// ── foreign: OS ops are externs (= procs). the fallible ones yield an `err` slot ──
#foreign {
  csv_read :: extern proc(path: []char, into: Transaction)(n: i32, err: i32)
  fopen    :: extern proc(path: []char, mode: i32)(fd: i32, err: i32)
  fwrite   :: extern proc(fd: i32, buf: []char)(n: i32, err: i32)
  fclose   :: extern proc(fd: i32)()
}

// ── funcs: pure. transforms, row builders, extern wrappers ──
bucket   :: map query { price, price_bucket } { price_bucket = price / 10.0; }

fmt_row  :: func(p: float, q: int, b: float) -> []char {
  return f32(p) <> "," <> i32(q) <> "," <> f32(b) <> "\n";
}

write_b  :: func(fd: i32, buf: []char) -> Eff(i32, i32) { return wrap fwrite(fd, buf); }

// ── each: effect leaf. uses a wrapped effect via `do`; per-row failure is captured as data ──
write_row :: each query { price, quantity, price_bucket, ok } (fd: i32) {
  do write_b(fd, fmt_row(price, quantity, price_bucket))(n:, err:);
  ok = (err == 0);
}

// ── systems: scheduled, no params. call externs/procs by name; branch on the runtime err ──
load :: system {
  csv_read("…/data_1k.csv", Transaction)(n:, err:);
  if err == 0 { run mark_loaded; }
}
compute :: system {
  run bucket;
}
write_out :: system {
  fopen("…/arche_output.csv", WRITE)(fd:, err:);   // OS op that can fail
  if err == 0 {
    run write_row(fd);
    fclose(fd);
  }
}

#schedule {
  once load;
  once compute;
  once write_out;
}
```

```arche
// ── funcs wrap and compose fallible externs into effect values ──
write_b :: func(fd: i32, buf: []char) -> Eff(i32, i32) { return wrap fwrite(fd, buf); }
line    :: func(fd: i32, s: []char)   -> Eff(i32, i32) { return write_b(fd, s <> "\n"); }

// ── proc: effect leaf. runs a wrapped effect via `do`, branches on the err it yields ──
log_header :: proc(fd: i32)(ok: bool) {
  do line(fd, "id,price,qty")(n:, err:);
  ok = (err == 0);
}

// ── system: calls externs/procs by name; sequences on the result ──
write_out :: system {
  fopen("…/out.csv", WRITE)(fd:, err:);
  if err == 0 {
    log_header(fd)(ok:);
    fclose(fd);
  }
}
```
