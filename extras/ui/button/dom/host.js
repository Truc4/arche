// Browser host for `button`'s dom backend — SHIPS WITH THE DEVICE. Fulfils button_be_label/poll with a real
// <button> (#ui-button) that mounts inside the panel frame (#ui-panel); its click raises a flag `poll` drains
// (edge-triggered, once per click). The window twin draws a rect + hit-tests the pointer.
(function () {
  (globalThis.archeHosts ??= []).push({
    bind(rt) {
      this.clicked = false;
      this.dec = new TextDecoder();
      let b = document.getElementById("ui-button");
      if (!b) {
        b = document.createElement("button");
        b.id = "ui-button";
        b.type = "button";
        b.style.cssText = "order:3;flex:0 0 auto;align-self:flex-start;box-sizing:border-box;cursor:pointer;" +
          "border:none;border-radius:0.4em;padding:0.55em 1.5em;background:#e4694e;color:#160d0a;" +
          "font:600 1em ui-sans-serif,system-ui,sans-serif;";
        (rt.root || document.body).appendChild(b);
      }
      b.addEventListener("click", () => { this.clicked = true; });
      this.b = b;
    },
    seams(rt) {
      const self = this;
      return {
        // Mount into the panel frame once it exists, then set the label (only on change, to preserve focus/press).
        button_be_label(ptr, n) {
          const f = document.getElementById("ui-panel");
          if (f && self.b.parentNode !== f) f.appendChild(self.b);
          const t = self.dec.decode(new Uint8Array(rt.memory().buffer, ptr, n));
          if (self.b.textContent !== t) self.b.textContent = t;
        },
        button_be_poll() { const f = self.clicked; self.clicked = false; return f ? 1 : 0; },
      };
    },
  });
})();
