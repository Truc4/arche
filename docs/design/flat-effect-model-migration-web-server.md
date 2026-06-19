# Migrating `arche-web-server` to the flat effect model

### a concrete before/after for [the-flat-effect-model](the-flat-effect-model.md)

> **Status & honesty note.** The flat effect model is a *design* (see
> [the-flat-effect-model.md](the-flat-effect-model.md)); the `Eff`/`system`/`#schedule` forms below are
> not yet implemented. The **"before"** snippets are the *real, current* source of
> `arche-web-server` (verbatim, with file:line). The **"after"** snippets are illustrative — they show
> how the same program restructures under the model, in its proposed syntax. The point isn't a
> mechanical rewrite; it's to see *what moves where* and *what gets easier*.

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

Then the effectful stdlib calls become `Eff`-returning builders (under-applied — no `wrap` keyword; the
absent out-slots *are* the `Eff`, per the model's saturation form):

```arche
recv_req  :: func(conn: socket, buf: []char)                            -> Eff(Request) { return net.recv(conn, buf); }
open_read :: func(path: []char)                                         -> Eff(int)     { return io.fopen_read(path); }
read_all  :: func(fd: int, buf: []char)                                 -> Eff([]char)  { return io.read(fd, buf); }
send      :: func(conn: socket, hdr: []char, st: int, ct: []char, b: []char, withbody: bool) -> Eff() { return http.respond(conn, hdr, st, ct, b, withbody); }
```

Now the proc is a thin imperative shell: run recv, call the pure planner, run the reply — and the *one*
genuinely result-dependent step (does the file open? then read it) stays inline, exactly where the model
says the monadic tail belongs:

```arche
handle :: proc(conn: socket, root: []char)(ok: bool) {
  reqbuf: [8192]char;
  hdr:    [1024]char;

  recv_req(conn, reqbuf)(req:);                                 // RUN recv
  if req.length <= 0 { ok = false; return; }

  r := plan(req, root);                                         // PURE: one call, whole decision

  if r.path.length == 0 {                                       // inline reply (health, errors)
    send(conn, hdr, r.status, r.ctype, r.body, true)(n:, err:);
    ok = (err == 0); return;
  }

  open_read(r.path)(fd:);                                       // result-dependent leaf…
  if !fd {
    send(conn, hdr, 404, "text/plain", "Not Found\n", true)(n:, err:);
    ok = (err == 0); return;
  }
  body: [262144]char;
  read_all(fd, body)(content:);                                // …its input (fd) was a prior output
  send(conn, hdr, 200, r.ctype, content, r.is_get)(n:, err:);
  ok = (err == 0);
}
```

What changed, precisely:

- **All routing/parsing/status logic** moved into `plan`, a pure func returning a value. You can test
  every route, every error path, the `..` rejection, GET-vs-HEAD — with a literal `Request` and an
  equality check. No socket, no disk, no server running. Today that same coverage needs a live server
  and a real client.
- **`serve_file` disappears as a separate proc.** Its pure parts (method check, `..` check, path build,
  MIME) folded into `plan`; its one impure, result-dependent part (open → read → send) folded into
  `handle`. This is the model's answer to "two procs shared an effectful step": you don't share a
  *sub-proc*, you share the pure planner and keep the thin run-it shell.
- **The result-dependent read stays a proc**, on purpose. `read_all`'s input is `open_read`'s output —
  that's the applicative→monadic boundary (§5 of the model). It cannot become an `Eff`; it cannot be a
  func; it lives imperatively in the one flat leaf. The migration makes the boundary *visible* instead
  of smearing it through `serve_file`.

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

### After — setup is a `once` system, the loop is the schedule

There is no `main`. Setup runs once; the per-connection work is a system the schedule loops. The listener
and root are singleton state (a 1-row pool — arche's idiom for a global) that the loop system reads:

```arche
[1]Server;   Server :: arche { srv :: socket   root :: []char }   // the singleton

startup :: system {
  os.argv(1)(portstr:);
  port := parse.atoi(portstr); if !port { port = 8000; }
  os.argv(2)(root:);
  register();
  net.listen(port)(srv:);
  fmt.printf("arche-web-server listening on port %d\n", port);
  insert(Server { srv: srv, root: root })(h:, ok:);              // stash the listener
}

serve :: system {                                                // the composer: no params
  accept(Server.srv[0])(conn:);                                  // run the accept leaf
  handle(conn, Server.root[0])(ok:);                             // run the handler proc; thread its result
}

#schedule {
  once startup;
  loop serve;
}
```

The `for(;;)` became `loop serve` in a declarative schedule. Every effect the server can perform is now
visible at two layers: the schedule (`startup` then forever `serve`) and the externs/primitives behind
`net`/`io`/`http`. Reading the schedule tells you the whole timeline — which is the legibility the model
trades flatness for.

---

## Where everything landed

| current | becomes | kind |
|---|---|---|
| `handle` (recv+parse+route+respond) | `plan` (pure) + `handle` (thin runner) | func + proc |
| `serve_file` (checks+path+open+read+respond) | folded: checks/path/MIME → `plan`; open/read/respond → `handle` | func + proc |
| `build_path` | unchanged — already a pure func | func |
| `register` | unchanged — pure router setup, called from `startup` | proc/func |
| `net.recv`/`io.*`/`http.respond` | `Eff`-returning builders (saturation) | func → `Eff` |
| `main` + `for(;;)` | `startup` (once) + `serve` (loop) | systems + `#schedule` |

The pure logic **did not get rewritten** — it got *relocated and relabelled*. Migration cost is low
precisely because a well-written imperative handler already has a pure core; the model just makes you
draw the line that was implicit.

---

## Honest assessment for *this* program

- **The clear win: testability.** `plan` turns the server's entire decision logic into one pure function
  over values. That's the difference between "spin up a server and curl it" and a table of
  `(Request) → Reply` assertions.
- **The clear win: a readable timeline.** `#schedule { once startup; loop serve; }` states the control
  flow declaratively; the effect set is the closed list of `net`/`io`/`http`/`os` primitives.
- **The honest small print:** this server is so I/O-shaped and so small that the *visible* restructuring
  is modest — one func extracted, one loop turned into a schedule. The model's leverage **grows with
  complexity**: add middleware, multiple backends, conditional pipelines, or per-route policies and the
  pure-planner / thin-runner split is what keeps it all testable and flat. For a 130-line blocking
  server, the migration is mostly *clarifying*, not *transformative* — which is itself a fair data point
  about when the model earns its keep.
- **The one thing it does *not* fix here:** concurrency. The model gives you a flat, legible,
  one-timeline server; it does not, by itself, make `serve` handle connections in parallel. That's the
  same open question as §9 (the schedule) in the main doc — multiple timelines are deliberately out of
  scope until something forces them.
