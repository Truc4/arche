```arche
// ── foreign: OS ops are externs (= procs). the fallible ones yield an `err` slot ──
#foreign {
  csv_read :: extern proc(path: []char, into: Transaction)(n: int, err: int)
  fopen    :: extern proc(path: []char, mode: int)(fd: int, err: int)
  fwrite   :: extern proc(fd: int, buf: []char)(n: int, err: int)
  fclose   :: extern proc(fd: int)()
}

// ── funcs: pure. transforms, row builders, extern wrappers ──
bucket   :: map query { price, price_bucket } { price_bucket = price / 10.0; }

fmt_row  :: func(p: float, q: int, b: float) -> []char {
  return f32(p) <> "," <> int(q) <> "," <> f32(b) <> "\n";
}

write_b  :: func(fd: int, buf: []char) -> Eff(int, int) { return wrap fwrite(fd, buf); }

// ── each: effect leaf. runs a wrapped effect by calling it with out-slots; per-row failure as data ──
write_row :: each query { price, quantity, price_bucket, ok } (fd: int) {
  write_b(fd, fmt_row(price, quantity, price_bucket))(n:, err:);
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
write_b :: func(fd: int, buf: []char) -> Eff(int, int) { return wrap fwrite(fd, buf); }
line    :: func(fd: int, s: []char)   -> Eff(int, int) { return write_b(fd, s <> "\n"); }

// ── proc: effect leaf. runs a wrapped effect (call it with out-slots), branches on the err it yields ──
log_header :: proc(fd: int)(ok: bool) {
  line(fd, "id,price,qty")(n:, err:);
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

```arche
// ── reacting to a NON-DETERMINISTIC runtime effect (the peer decides what/when) ──
#foreign {
  accept :: extern proc(port: int)(fd: int, err: int)                      // which client, when
  recv   :: extern proc(fd: int, buf: []char, n: int)(got: int, err: int)  // returns whatever arrived
}

// pure: wrap the non-deterministic externs into effect values (build, run nothing)
listen :: func(port: int)                  -> Eff(int, int) { return wrap accept(port); }
read   :: func(fd: int, buf: []char, n: int) -> Eff(int, int) { return wrap recv(fd, buf, n); }

// procs = flat effect LEAVES: run a wrapped effect (call with out-slots) and REACT. No proc calls a proc.
open_conn :: proc(port: int)(fd: int, ok: bool) {
  listen(port)(fd:, err:);                  // ← non-deterministic: which client, when
  ok = (err == 0);                          // ← react
}
handle_msg :: proc(fd: int)(ok: bool) {
  hdr: [1]char;
  read(fd, hdr, 1)(got:, err:);             // ← non-deterministic read
  if err != 0 || got == 0 {                  // ← react: error, or the peer hung up
    ok = false;
    return;
  }
  len := int(hdr[0]);                        // the peer chose this length at runtime
  body: [256]char;
  read(fd, body, len)(got:, err:);           // ← dependent read: its shape decided by the first
  ok = (err == 0 && got == len);             // ← react to the second result
}

// system = the COMPOSER: runs leaves and threads their out-slots (fd: open_conn → handle_msg). It
// sequences and branches on results, but performs NO effect itself — the doing lives in the procs.
serve :: system {
  open_conn(8080)(fd:, ok:);
  if ok { handle_msg(fd)(done:); }
}

#schedule { loop serve; }
```
