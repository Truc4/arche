(function () {
  (globalThis.archeHosts ??= []).push({
    bind(rt) {
      this.dec = new TextDecoder();
      let f = document.getElementById("ui-panel");
      if (!f) {
        f = document.createElement("div");
        f.id = "ui-panel";
        // No flexbox/padding: children are absolutely positioned from the driver's projected rects (the same
        // `layout` the native window backend reads), so native + browser lay out identically.
        f.style.cssText = "position:absolute;z-index:5;box-sizing:border-box;background:#0b0e14;" +
          "border:1px solid #232838;border-radius:0.6em;box-shadow:0 10px 34px rgba(0,0,0,0.5);overflow:hidden;";
        const t = document.createElement("div");
        t.id = "ui-panel-title";
        t.style.cssText = "position:absolute;font:700 1.15em/1 ui-sans-serif,system-ui,sans-serif;" +
          "letter-spacing:0.08em;color:#cdd6f4;white-space:nowrap;";
        f.appendChild(t);
        (rt.root || document.body).appendChild(f);
      }
      this.frame = f;
    },

    seams(rt) {
      const self = this;
      return {
        panel_be_render(x, y, w, h, ptr, n) {
          const s = window.innerHeight / (rt.renderH || 1080), f = self.frame;
          // Publish the panel origin + scale so each element host can position itself relative to the panel.
          // panel.render runs before the element renders each frame, so these are fresh. (18 = ui.PAD.)
          rt._uiScale = s; rt._uiPanelX = x; rt._uiPanelY = y;
          f.style.left = (x * s) + "px";
          f.style.top = (y * s) + "px";
          f.style.width = (w * s) + "px";
          f.style.height = (h * s) + "px";
          f.style.fontSize = (20 * s) + "px";
          const t = self.dec.decode(new Uint8Array(rt.memory().buffer, ptr, n));
          const tt = document.getElementById("ui-panel-title");
          if (tt) {
            if (tt.textContent !== t) tt.textContent = t;
            tt.style.left = (18 * s) + "px";
            tt.style.top = (18 * s) + "px";
          }
        },
      };
    },
  });
})();
