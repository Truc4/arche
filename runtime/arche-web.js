// arche-web.js — the generic browser runtime for Arche wasm. SHIPS WITH THE COMPILER; `arche build
// --arch=wasm32` copies it next to the `.wasm` alongside the emitted `<out>.hosts.js` (the device browser
// hosts). It: (1) bundles a minimal WASI preview1 shim, (2) assembles the seams every registered device host
// exposes into the wasm `env`, (3) instantiates the module, and (4) DRIVES it by shape — a reactor
// (`arche_frame` exported) via requestAnimationFrame, or a one-shot command (`_start`). The app then needs
// only:  <script src="arche-web.js"><script src="X.hosts.js"><script>archeRun("X.wasm")</script>  — no
// per-device JS. Device authors ship a `host.js` next to their wasm/dom `backend.arche` that does:
//   (globalThis.archeHosts ??= []).push({ bind(rt){…}, seams(rt){ return { dev_be_x(){…} }; } });
(function (global) {
  const OK = 0, EBADF = 8, ENOSYS = 52, FT_CHAR = 2;
  class WasiExit { constructor(code) { this.code = code; } }

  class WasiShim {
    constructor(args) { this.args = args && args.length ? args : ["arche"]; this.stdout = ""; this.stderr = ""; this.memory = null; this._dec = new TextDecoder(); this._enc = new TextEncoder(); }
    _dv() { return new DataView(this.memory.buffer); }
    _u8() { return new Uint8Array(this.memory.buffer); }
    get imports() {
      const self = this;
      return { wasi_snapshot_preview1: {
        fd_write(fd, iovsPtr, iovsLen, nwrittenPtr) {
          const dv = self._dv(), u8 = self._u8(); let total = 0, s = "";
          for (let i = 0; i < iovsLen; i++) { const p = iovsPtr + i * 8, buf = dv.getUint32(p, true), len = dv.getUint32(p + 4, true); s += self._dec.decode(u8.subarray(buf, buf + len)); total += len; }
          if (fd === 2) self.stderr += s; else self.stdout += s;
          dv.setUint32(nwrittenPtr, total, true); return OK;
        },
        fd_read() { return EBADF; }, fd_close() { return OK; }, fd_seek() { return ENOSYS; },
        fd_fdstat_get(fd, p) { const dv = self._dv(); dv.setUint8(p, FT_CHAR); dv.setUint16(p + 2, 0, true); dv.setBigUint64(p + 8, 0xffffffffffffffffn, true); dv.setBigUint64(p + 16, 0xffffffffffffffffn, true); return OK; },
        fd_fdstat_set_flags() { return OK; }, fd_prestat_get() { return EBADF; }, fd_prestat_dir_name() { return EBADF; }, path_open() { return ENOSYS; },
        args_sizes_get(aP, bP) { const dv = self._dv(); let b = 0; for (const a of self.args) b += self._enc.encode(a).length + 1; dv.setUint32(aP, self.args.length, true); dv.setUint32(bP, b, true); return OK; },
        args_get(avP, abP) { const dv = self._dv(), u8 = self._u8(); let p = abP; for (let i = 0; i < self.args.length; i++) { dv.setUint32(avP + i * 4, p, true); for (const ch of self._enc.encode(self.args[i])) u8[p++] = ch; u8[p++] = 0; } return OK; },
        environ_sizes_get(cP, bP) { const dv = self._dv(); dv.setUint32(cP, 0, true); dv.setUint32(bP, 0, true); return OK; },
        environ_get() { return OK; },
        clock_time_get(id, prec, tP) { self._dv().setBigUint64(tP, BigInt(Date.now()) * 1000000n, true); return OK; },
        clock_res_get(id, rP) { self._dv().setBigUint64(rP, 1000000n, true); return OK; },
        random_get(bP, n) { globalThis.crypto.getRandomValues(self._u8().subarray(bP, bP + n)); return OK; },
        poll_oneoff() { return ENOSYS; }, sched_yield() { return OK; }, proc_exit(code) { throw new WasiExit(code); },
      } };
    }
  }

  async function archeRun(wasmUrl, opts) {
    opts = opts || {};
    const rt = { _mem: null, _stopped: false, root: opts.root || document.body, memory: () => rt._mem, stdout: "" };
    const hosts = global.archeHosts || [];
    for (const h of hosts) if (h.bind) h.bind(rt);
    // Base seams the runtime provides for EVERY program (core diagnostics, not device-specific). A device
    // host may override them. `log_be_emit` is the panic/log seam core links into every module.
    const base = {
      log_be_emit(level, ptr, len) {
        try { const s = new TextDecoder().decode(new Uint8Array(rt.memory().buffer, ptr, len)); (level >= 2 ? console.error : console.log)(s.replace(/\n$/, "")); } catch (e) {}
      },
    };
    const env = Object.assign(base, ...hosts.map((h) => (h.seams ? h.seams(rt) : {})));
    const wasi = new WasiShim(opts.args || ["arche"]);
    const bytes = wasmUrl instanceof Uint8Array ? wasmUrl : new Uint8Array(await (await fetch(wasmUrl)).arrayBuffer());
    const { instance } = await WebAssembly.instantiate(bytes, Object.assign({}, wasi.imports, { env }));
    const ex = instance.exports;
    rt._mem = wasi.memory = ex.memory;
    if (typeof ex.arche_frame === "function") { // reactor: init once, then a frame per rAF
      if (typeof ex._initialize === "function") ex._initialize();
      if (typeof ex.arche_run === "function") ex.arche_run();
      const tick = () => { if (rt._stopped) return; try { ex.arche_frame(); } catch (e) { rt._stopped = true; return; } rt._raf = requestAnimationFrame(tick); };
      rt._raf = requestAnimationFrame(tick);
    } else { // command: run once
      try { ex._start(); } catch (e) { if (!(e instanceof WasiExit) || e.code !== 0) { rt.stdout = wasi.stdout; throw e; } }
    }
    rt.stdout = wasi.stdout;
    rt.stop = () => { rt._stopped = true; if (rt._raf) cancelAnimationFrame(rt._raf); };
    return rt;
  }

  global.archeRun = archeRun;
  global.WasiShim = WasiShim;
})(typeof window !== "undefined" ? window : globalThis);
