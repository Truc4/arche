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
        ta.style.cssText = "position:absolute;box-sizing:border-box;resize:none;background:#0e121b;" +
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
        textedit_be_text(bufPtr, cap) {
          const bytes = self.enc.encode(self.ta.value);
          const k = Math.min(bytes.length, cap - 1);
          const mem = rt.memory();
          new Uint8Array(mem.buffer, bufPtr, cap).set(bytes.subarray(0, k));
          new Uint8Array(mem.buffer)[bufPtr + k] = 0;
        },
        textedit_be_poll_run() { const f = self.runPending; self.runPending = false; return f ? 1 : 0; },
        // Position the <textarea> from the driver's projected rect (render px), relative to the panel origin.
        textedit_be_place(x, y, w, h) {
          const s = rt._uiScale || window.innerHeight / (rt.renderH || 1080);
          const ta = self.ta;
          ta.style.left = ((x - (rt._uiPanelX || 0)) * s) + "px";
          ta.style.top = ((y - (rt._uiPanelY || 0)) * s) + "px";
          ta.style.width = (w * s) + "px";
          ta.style.height = (h * s) + "px";
        },
      };
    },
  });
})();
