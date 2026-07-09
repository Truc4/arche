(function () {
  (globalThis.archeHosts ??= []).push({
    bind(rt) {
      this.dec = new TextDecoder();
      let f = document.getElementById("ui-panel");
      if (!f) {
        f = document.createElement("div");
        f.id = "ui-panel";
        f.style.cssText = "position:absolute;z-index:5;box-sizing:border-box;display:flex;flex-direction:column;" +
          "gap:0.6em;padding:0.85em;background:#0b0e14;border:1px solid #232838;border-radius:0.6em;" +
          "box-shadow:0 10px 34px rgba(0,0,0,0.5);overflow:hidden;";
        const t = document.createElement("div");
        t.id = "ui-panel-title";
        t.style.cssText = "order:0;flex:0 0 auto;font:700 1.15em/1 ui-sans-serif,system-ui,sans-serif;" +
          "letter-spacing:0.08em;color:#cdd6f4;padding:0.15em 0.1em 0.55em;border-bottom:1px solid #232838;";
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
          f.style.left = (x * s) + "px";
          f.style.top = (y * s) + "px";
          f.style.width = (w * s) + "px";
          f.style.height = (h * s) + "px";
          f.style.fontSize = (20 * s) + "px";
          const t = self.dec.decode(new Uint8Array(rt.memory().buffer, ptr, n));
          const tt = document.getElementById("ui-panel-title");
          if (tt && tt.textContent !== t) tt.textContent = t;
        },
      };
    },
  });
})();
