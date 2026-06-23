# Migrating `arche-web-server` to the flat effect model

### a concrete before/after for [the-flat-effect-model](the-flat-effect-model.md)

> **Status & honesty note.** The flat effect model is a *design* (see
> [the-flat-effect-model.md](the-flat-effect-model.md)); the `Eff`/`system`/`#run` forms below are
> not yet implemented. The **"before"** snippets are the *real, current* source of
> `arche-web-server` (verbatim, with file:line). The **"after"** snippets are illustrative — they show
> how the same program restructures under the model, in its proposed syntax. The point isn't a
> mechanical rewrite; it's to see *what moves where* and *what gets easier*. The per-wrapper conversion
> recipe is [flat-effect-model-stdlib-migration.md](flat-effect-model-stdlib-migration.md).

---

## The program in one breath

`arche-web-server` is a blocking single-threaded static file server: `listen`, then a `for(;;)` loop
that `accept`s a connection and `handle`s it — `recv` the request, parse it, route it, and either reply
inline (`/health`, errors) or read a file off disk and send it. Two source files, ~130 lines.

Its entire effect surface (every touch of the outside world):

| effect | current call | `src/…:line` |
|---|---|---|
| read CLI args | `os.argv(1)(portstr:)` | `main.arche:6,11` |
| open listener | `net.listen(port)(srv:)` | `main.arche:15` |
| log startup | `fmt.printf(...)` | `main.arche:16` |
| accept conn | `net.accept(srv)(conn:)` | `main.arche:19` |
| recv request | `net.recv(conn, reqbuf)(req:)` | `server.arche:15` |
| send response | `http.respond[_text](...)` | `server.arche:22,32,48,53,69` |
| open file | `io.fopen_read(pathv)(fd:)` | `server.arche:60` |
| read file | `io.read(fd, body)(content:)` | `server.arche:67` |

Everything else — request-line parsing, routing, MIME lookup, path assembly (`build_path`) — is already
pure. That's the key observation that makes the migration cheap: **the pure core already exists; it's
just interleaved with I/O rather than separated from it.**

---

## Before/after 1 — the request handler

This is the centerpiece. Today `handle` interleaves recv (I/O), parse (pure), route (pure), and respond
(I/O) in one proc.

### Before — `src/server.arche:11–38` (verbatim)

```arche
handle :: proc(conn: socket, root: []char) {
  reqbuf : [8192]char;
  hdr    : [1024]char;

  net.recv(conn, reqbuf)(req:);                                   // I/O
  if (req.length <= 0) { return; }

  http.parse_request_line(req)(method:, target:, ok:);           // pure
  if (!ok) {
    http.respond_text(conn, hdr, 400, "Bad Request\n");          // I/O
    return;
  }

  router.resolve(target)(id:);                                   // pure
  if (id < 0) { id = route.files; }
  match id {
  route.health: {
    http.respond(conn, hdr, 200, "text/plain; charset=utf-8", "ok\n", 1);   // I/O
  }
  route.files: {
    serve_file(conn, method, target, root);                     // mixed (see #2)
  }
  }
}
```

### After — the decision becomes one pure func; the proc only runs effects

First, lift "what should the reply be?" out of the proc entirely. It's all pure — parse, route, choose a
status/content-type, and (for the file route) compute the path. It returns a **plan**, a value:

```arche
// PURE: the whole decision, no socket, no disk. Unit-testable with zero mocks.
Reply :: arche {
  status :: int
  ctype  :: []char
  body   :: []char     // inline body — empty when serving a file
  path   :: []char     // file to serve — empty when replying inline
  is_get :: bool
}

plan :: func(req: Request, root: []char) -> Reply {
  http.parse_request_line(req)(method:, target:, ok:);          // (now a pure parse → values)
  if !ok                      { return Reply { status: 400, ctype: "text/plain", body: "Bad Request\n" }; }
  router.resolve(target)(id:);
  if id == route.health       { return Reply { status: 200, ctype: "text/plain", body: "ok\n" }; }

  is_get  := streq(method, "GET");
  is_head := streq(method, "HEAD");
  if !is_get && !is_head      { return Reply { status: 405, ctype: "text/plain", body: "Method Not Allowed\n" }; }
  if str.contains(target,"..") { return Reply { status: 403, ctype: "text/plain", body: "Forbidden\n" }; }

  path: [3072]char;
  pathv := build_path(move path, root, target);                 // already a pure func today (server.arche:72)
  return Reply { status: 200, ctype: http.mime_by_ext(pathv), path: pathv, is_get: is_get };
}
```

Then the effectful steps run the stdlib directly — but **not** by wrapping the stdlib procs. `net.recv`,
`io.fopen_read`, `io.read`, `http.respond` are stdlib *procs* today, and a func may not wrap a proc
(that's the proc→proc loophole the model forbids — wrapping `http.respond` in a func is just
proc→proc with extra steps). The fix lives in the **stdlib**, not the app: the convenience layer itself
becomes funcs→`Eff` that wrap the *real* leaves — the `net_*` externs and `os.syscall` — never another
proc (see [§4 of the model](the-flat-effect-model.md)). So the app defines **no** wrappers at all; it
just calls a now-`Eff`-returning stdlib.

What that rewrite looks like in the lib (faithful to the current externs/syscalls — `net_recv`/`net_send`
are `#foreign` externs; `io.*` bottom out at `os.syscall`):

Each wrapper is `func → Eff(<the primitive's raw out-slots>)` — the lib builds the effect, the caller
cooks the raw result. The status/count is just an out-slot the caller branches on; the buffer is the
caller's, so any slice happens at the call site. No `Result` type, no error-folding finalizer:

```arche
// stdlib/net — wrap the EXTERN net_recv; raw out-slots are (the buffer view, the count/errno)
recv :: func(s: socket, buf: []char) -> Eff([]char, int) {
  return net_recv(s, buf, buf.length);
}

// stdlib/io — io.read / io.fopen_read bottom out at os.syscall (the prim); the raw count / fd is the out-slot
read :: func(f: fd, buf: []char) -> Eff(i64) {
  return os.syscall(0, f, buf, buf.length, 0, 0, 0);
}
fopen_read :: func(path: []char) -> Eff(i64) {
  return os.syscall(2, path, 0, 0, 0, 0, 0);
}

// stdlib/http — head is pure bytes; each send is a func→Eff over the EXTERN; `respond` composes the two
// independent sends with `seq` (applicative — neither send depends on the other's result).
head :: func(hdr: []char, status: int, ctype: []char, body_len: int) -> []char {
  fmt.sprintf(_, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
              status, reason(status), ctype, body_len)(hdr, n:);
  return hdr[0: n];                                                   // pure: the head bytes
}
send_bytes :: func(conn: socket, data: []char) -> Eff([]char, int) {
  return net_send(conn, data, data.length);
}
respond :: func(conn: socket, hdr: []char, status: int, ctype: []char, body: []char, send_body: bool) -> Eff([]char, int) {
  h := head(hdr, status, ctype, body.length);
  return send_body ? seq(send_bytes(conn, h), send_bytes(conn, body))  // applicative: sends are independent
                   : send_bytes(conn, h);
}
```

(`seq` = applicative sequence of two independent `Eff`s, yielding the last. The *result-dependent*
`io.fread_line` is the one stdlib proc this rewrite can't reach — it stays monadic; its long-term home is
the `routine` construct, §9 of the model.)

Now the proc is a thin imperative shell: run recv, call the pure planner, run the reply — and the *one*
genuinely result-dependent step (does the file open? then read it) stays inline, exactly where the model
says the monadic tail belongs:

```arche
handle :: proc(conn: socket, root: []char)(ok: bool) {
  reqbuf: [8192]char;
  hdr:    [1024]char;

  net.recv(conn, reqbuf)(req:, n:);                            // RUN: raw out-slots — buffer view + count/errno
  if n <= 0 { ok = false; return; }                           // react to the status as data — no exception
  msg := req[0: n];                                           // caller cooks: slice its own buffer

  r := plan(msg, root);                                       // PURE: one call, whole decision

  if r.path.length == 0 {                                     // inline reply (health, errors)
    http.respond(conn, hdr, r.status, r.ctype, r.body, true)(_:, sent:);
    ok = sent >= 0; return;
  }

  io.fopen_read(r.path)(fdv:);                               // result-dependent leaf: raw fd / negative errno
  if fdv < 0 {                                               // open failed → 404, as data
    http.respond(conn, hdr, 404, "text/plain", "Not Found\n", true)(_:, _:); ok = false; return;
  }
  body: [262144]char;
  io.read(fd(fdv), body)(rn:);                               // …its input (fd) was a prior output
  http.respond(conn, hdr, 200, r.ctype, body[0: rn], r.is_get)(_:, sent:);
  ok = sent >= 0;
}
```

What changed, precisely:

- **All routing/parsing/status logic** moved into `plan`, a pure func returning a value. You can test
  every route, every error path, the `..` rejection, GET-vs-HEAD — with a literal `Request` and an
  equality check. **Be precise about the size of this win, though:** most of that logic is *already* pure
  and *already* tested without a socket today — `parse_request_line` ships with a doctest
  (`stdlib/http/http.arche`) and a unit test (`tests/unit/.../request_line.arche`); `router.resolve`,
  `build_path`, `mime_by_ext`, `reason` are already `func`s. What `plan` adds is only the *glue* (status
  selection) those leave out — and extracting that into a pure `func` returning a `Reply` is "extract a
  function," which the language already permits with **no** `Eff`, no model, no migration. The model
  contributes nothing here that a struct return doesn't. (The genuinely model-specific testability lever —
  swapping the executor `system` to replay a recorded tape, §6 — this program has no use for.)
- **`serve_file` disappears as a separate proc.** Its pure parts (method check, `..` check, path build,
  MIME) folded into `plan`; its one impure, result-dependent part (open → read → send) folded into
  `handle`. This is the model's answer to "two procs shared an effectful step": you don't share a
  *sub-proc*, you share the pure planner and keep the thin run-it shell.
- **The result-dependent read stays a proc**, on purpose. `io.read`'s input (`fd`) is `io.fopen_read`'s
  output — that's the applicative→monadic boundary (§5 of the model). That *sequencing* cannot become an
  `Eff` and cannot be a func; it lives imperatively in the one flat leaf. (Each individual call —
  `net.recv`, `io.fopen_read`, `io.read`, `http.respond` — is now itself a func→`Eff` built in the
  stdlib over a real primitive; what stays in the proc is only the *order-with-dependency* between them.)
  The migration makes the boundary *visible* instead of smearing it through `serve_file`.

---

## Before/after 2 — the server loop

### Before — `src/main.arche:5–22` (verbatim)

```arche
main :: proc() {
  os.argv(1)(portstr:);
  port := parse.atoi(portstr);
  if (!port) { port = 8000; }
  os.argv(2)(root:);

  register();

  net.listen(port)(srv:);
  fmt.printf("arche-web-server listening on port %d\n", port);

  for (;;) {
    net.accept(srv)(conn:);
    handle(conn, root);
  }
}
```

### After — one connection is one system; the runtime paces it

There is no `main`, no driver, no `for(;;)`. A program is its declarations plus one **`#run <Schedule>`**,
and the **runtime** owns the loop. The per-connection work is a `system` (`serve`): accept, handle. Setup —
parsing the port, registering routes, opening the listener, seeding the singleton — is a second `system`
(`bind`) the schedule runs *once* before the loop. The listener and root live in singleton state (a 1-row
pool — arche's idiom for a global) the systems read:

```arche
[1]Server;   Server :: arche { srv :: socket   root :: []char }   // the singleton

bind :: system {                                                 // setup: run ONCE, then the loop
  os.argv(1)(portstr:);
  port := parse.atoi(portstr); if !port { port = 8000; }
  os.argv(2)(root:);
  register();
  net.listen(port)(srv:);
  insert(Server { srv: srv, root: root })(h:, ok:);
}

serve :: system {                                                // ONE connection's work
  accept(Server.srv[0])(conn:);                                  // run the accept leaf (blocks → event-paced)
  handle(conn, Server.root[0])(ok:);                             // run the handler proc; thread its result
}

#run seq({ run(bind), forever(run(serve)) })               // bind once, then serve forever
```

The loop isn't a statement anyone writes — it's the `forever(run(serve))` value, and advancing it is the
runtime's job. A bare system is *not* a `Schedule`; a system enters one explicitly through `run(serve)`, the
leaf that holds `serve`'s compile-time identity. `seq` takes a `[]Schedule` array literal — `run(bind)`
then `forever(run(serve))` — so "set up, then loop" is a single composed value, not a phase construct.
`forever` and `once` aren't keywords; they're ordinary pure funcs returning `Schedule`
(`forever(s) = loop(s)`, `once(s) = seq({ s, halt })`), composed in the value plane like anything else.

The server is event-paced: `accept` blocks until a connection arrives, so each iteration of `forever` is one
connection. No clock, no `when` guard — just `forever`. Every effect is still visible at two layers — the
schedule (`bind`, `serve`) and the externs behind `net`/`io`/`http` — and there is nothing above the systems
but the runtime walking the one `Schedule` value.

---

## Where everything landed

| current | becomes | kind |
|---|---|---|
| `handle` (recv+parse+route+respond) | `plan` (pure) + `handle` (thin runner) | func + proc |
| `serve_file` (checks+path+open+read+respond) | folded: checks/path/MIME → `plan`; open/read/respond → `handle` | func + proc |
| `build_path` | unchanged — already a pure func | func |
| `register` | unchanged — pure router setup, called from the `bind` system | proc/func |
| `net.recv`/`io.*`/`http.respond` (stdlib **procs** today) | rewritten *in the stdlib* as funcs→`Eff` wrapping the real leaves (`net_*` externs, `os.syscall`) — never wrapping a proc | func → `Eff` |
| `io.fread_line` (result-dependent loop) | can't be a func/`Eff`; stays monadic — the `routine` holdout (§9) | proc / future `routine` |
| `main` + `for(;;)` | `bind` system (setup) + `serve` system (one connection); runtime loops via `#run seq({ run(bind), forever(run(serve)) })` | system + `Schedule` |

The pure logic **did not get rewritten** — it got *relocated and relabelled*. Migration cost is low
precisely because a well-written imperative handler already has a pure core; the model just makes you
draw the line that was implicit.

---

## Honest assessment for *this* program

An honest accounting for this specific program:

- **The model's engine never turns over here.** Its real payoff is the *effect column* — many independent
  effects built in a pure pass and drained in one fanned `each` kernel (§6 of the model). This server
  handles **one connection at a time**, one sequential I/O chain. There is no fan-out, no column, no
  batch. So it pays the model's setup cost for leverage it never exercises. (Contrast the ETL migration,
  where the write *is* a column of N effects — that's where the engine runs.)
- **The testability "win" is mostly pre-existing.** The routing/parsing logic `plan` extracts is *already*
  pure and *already* tested without a socket: `parse_request_line` ships a doctest + unit test;
  `router.resolve`/`build_path`/`mime_by_ext`/`reason` are already `func`s. `plan` only adds the status-
  selection glue — and lifting that to a pure func is "extract a function," which the language already
  allows with **no** `Eff`, no model. The model contributes nothing here a struct return doesn't.
- **The one real structural win is the readable timeline.** The `Schedule`
  (`#run seq({ run(bind), forever(run(serve)) })`) names the per-connection work and its
  setup-then-loop pacing as one value, the runtime drives it, and the effect set is the closed
  `net`/`io`/`http`/`os` primitive list. Modest, but genuine.
- **The cost is real and program-wide — don't spin it.** Making `net.recv`/`io.read`/`http.respond`
  composable means rewriting `net`/`io`/`http`/`os` — the modules *every* arche program imports — from
  `proc(...)(out:)` to `func → Eff`. That ripples to every caller, and leaves a **mixed** stdlib: half
  `Eff`-returning funcs, half irreducibly-monadic procs (`io.fread_line` and its callers can't convert,
  §9). A maintainer must now track which is which. That is a larger maintenance surface than today's
  uniform "everything's a proc with out-slots," and calling it a "forcing function" doesn't make the bill
  smaller.
- **Streaming/chunked responses get *harder*.** A chunked writer is result-dependent (write a chunk, check
  it, write the next) — the monadic case the model forbids from being a func or `Eff`, and procs aren't
  values, so it **can't be factored** into a shared helper. Today you could write `stream_file` once and
  call it; the model removes that option until the `routine` construct exists (§9).
- **Where it *would* win: the concurrent version.** Model N connections as a `Connection` pool and the
  server becomes device-systems over a column — `accept`/`recv`/`send` as systems, readiness as event
  rows (epoll-as-ECS), result-dependent per-connection protocol state as a defunctionalized state-column.
  *That* exercises the engine. The blocking, one-at-a-time server is the degenerate case that doesn't —
  which is the honest reason this is the model's weakest demo, not its showcase.
