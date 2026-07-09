// Browser host for `textedit`'s dom backend — SHIPS WITH THE DEVICE. Fulfils textedit_be_* with a real
// <textarea> (#ui-textedit) that mounts inside the panel frame (#ui-panel). `open` seeds it from the driver's
// buffer + focuses; `text` copies its value into the driver buffer; `poll_run` reports ⌘/Ctrl-Enter. The window
// twin edits + draws the same buffer in the framebuffer.
(function () {
  (globalThis.archeHosts ??= []).push({
    bind(rt) {
      this.runPending = false;
      this.dec = new TextDecoder();
      this.enc = new TextEncoder();
      let ta = document.getElementById("ui-textedit");
      if (!ta) {
        ta = document.createElement("textarea");
        ta.id = "ui-textedit";
        ta.spellcheck = false;
        ta.setAttribute("autocomplete", "off");
        ta.style.cssText = "order:1;height:22em;width:100%;box-sizing:border-box;resize:none;background:#0e121b;" +
          "color:#cdd6f4;border:1px solid #232838;border-radius:0.4em;padding:0.7em;" +
          "font:1em/1.5 ui-monospace,Menlo,monospace;outline:none;";
        (rt.root || document.body).appendChild(ta);
      }
      ta.addEventListener("keydown", (e) => {
        if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) { this.runPending = true; e.preventDefault(); }
      });
      this.ta = ta;
    },
    seams(rt) {
      const self = this;
      return {
        // Mount into the panel frame; seed the textarea from the driver's buffer once (read to the NUL); focus.
        textedit_be_open(ptr, n) {
          const f = document.getElementById("ui-panel");
          if (f && self.ta.parentNode !== f) f.appendChild(self.ta);
          if (!self.ta.value) {
            const mem = new Uint8Array(rt.memory().buffer, ptr, n);
            let end = 0;
            while (end < n && mem[end] !== 0) end++;
            self.ta.value = self.dec.decode(mem.subarray(0, end));
          }
          self.ta.focus();
        },
        // textarea value → the driver buffer, NUL-terminated (the arche side rescans elen).
        textedit_be_text(bufPtr, cap) {
          const bytes = self.enc.encode(self.ta.value);
          const k = Math.min(bytes.length, cap - 1);
          const mem = rt.memory();
          new Uint8Array(mem.buffer, bufPtr, cap).set(bytes.subarray(0, k));
          new Uint8Array(mem.buffer)[bufPtr + k] = 0;
        },
        textedit_be_poll_run() { const f = self.runPending; self.runPending = false; return f ? 1 : 0; },
      };
    },
  });
})();
