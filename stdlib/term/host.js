// Browser host for the stdlib `term` device — the wasm twin of runtime/term.c (termios). SHIPS WITH THE
// DEVICE; `arche build --arch=wasm32` collects it (term is single-impl, so this lives in the top folder and
// is emitted for wasm builds). Fulfils term_raw_enable/term_raw_restore/term_read_key: a `keydown` handler
// pushes bytes into a queue; `term_read_key` shifts the next byte (0 when empty — byte-identical to native).
(globalThis.archeHosts ??= []).push({
  bind(rt) {
    this.queue = [];
    const named = { Backspace: 127, Enter: 13, Tab: 9, Escape: 27, ArrowLeft: 27, ArrowRight: 27 };
    const push = (e) => {
      let b = named[e.key];
      if (b === undefined && e.key && e.key.length === 1) b = e.key.charCodeAt(0);
      if (b !== undefined) { this.queue.push(b); e.preventDefault(); }
    };
    if (typeof addEventListener === "function") addEventListener("keydown", push);
    rt.termQueue = this.queue; // exposed so a headless/test driver can inject bytes
  },
  seams(rt) {
    const self = this;
    return {
      term_raw_enable() {},   // the browser has no tty to put in raw mode
      term_raw_restore() {},
      term_read_key() { return self.queue.length ? self.queue.shift() : 0; }, // proc()(k: int) → returns i32
    };
  },
});
