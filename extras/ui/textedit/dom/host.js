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
          // Deliberately NOT self.ta.focus(). `open` runs once at boot, so focusing here handed the editor the
          // keyboard before the user had asked for it — and gfx goes silent whenever a text field is focused,
          // so the app booted with the world unable to move until you clicked the canvas. Worse, the editor is
          // often positioned off-screen (it lives at a world anchor), and arrow keys landing on a focused
          // off-screen element make the browser scroll its container to reveal it, dragging the canvas away.
          // The world owns the keyboard by default; the user clicks the editor to take it.
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
