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
        // The driver's rect is already in SCREEN space, so place it absolutely in the app root — do NOT reparent
        // into the panel div and subtract the panel's origin. That coupling made the element depend on the panel
        // existing FIRST, and the panel is now created lazily on its first render, which happens after `open`
        // runs at boot: the element never found it, stayed a child of the root, and had panel-relative
        // coordinates applied absolutely — so it sat glued to the screen instead of scrolling with the world.
        // Depth is echoed from the driver's `z`, keeping it above the foreground panel it visually sits in.
        textedit_be_place(x, y, w, h, z) {
          const s = rt._uiScale || window.innerHeight / (rt.renderH || 1080);
          const ta = self.ta;
          ta.style.zIndex = z;
          ta.style.left = (x * s) + "px";
          ta.style.top = (y * s) + "px";
          ta.style.width = (w * s) + "px";
          ta.style.height = (h * s) + "px";
        },
      };
    },
  });
})();
