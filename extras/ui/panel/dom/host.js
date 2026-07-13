(function () {
  (globalThis.archeHosts ??= []).push({
    bind(rt) {
      this.dec = new TextDecoder();
      // ONE REAL <div> PER `bid`. This used to be a hardcoded singleton (getElementById("ui-panel")), so a
      // driver with two Panel rows saw the second silently reposition the first.
      this.panels = new Map();
    },

    seams(rt) {
      const self = this;

      const get = (bid, layer) => {
        let f = self.panels.get(bid);
        if (f) return f;
        // bid 0 keeps the id "ui-panel": the editor / output hosts reparent themselves into it BY THAT ID, so
        // it is the one panel that hosts children. Every other panel is chrome.
        const id = bid === 0 ? "ui-panel" : "ui-panel-" + bid;
        let el = document.getElementById(id);
        if (!el) {
          el = document.createElement("div");
          el.id = id;
          // No flexbox/padding: children are absolutely positioned from the driver's projected rects (the same
          // `layout` the native window backend reads), so native + browser lay out identically.
          //
          // The z-index STRADDLES the two gfx canvases (see gfx.arche's `split`). A background panel gets 1 —
          // above the background canvas, but BELOW the transparent foreground one, so the world's bodies draw
          // over it. A foreground panel gets 5, above both. That is the whole trick: DOM always paints above a
          // canvas, so the only way a DOM panel can sit behind the world is for the world to have a second
          // canvas on top.
          //
          // A background panel is scenery, so it must not eat pointer events either — a click on it has to fall
          // through to the world underneath it.
          const isBg = layer === 0;
          el.style.cssText = "position:absolute;box-sizing:border-box;background:#0b0e14;" +
            "border:1px solid #232838;border-radius:0.6em;box-shadow:0 10px 34px rgba(0,0,0,0.5);overflow:hidden;" +
            "z-index:" + (isBg ? 1 : 5) + ";" + (isBg ? "pointer-events:none;" : "");
          const t = document.createElement("div");
          t.id = id + "-title";
          t.style.cssText = "position:absolute;font:700 1.15em/1 ui-sans-serif,system-ui,sans-serif;" +
            "letter-spacing:0.08em;color:#cdd6f4;white-space:nowrap;";
          el.appendChild(t);
          (rt.root || document.body).appendChild(el);
        }
        f = { el: el, title: document.getElementById(id + "-title") };
        self.panels.set(bid, f);
        return f;
      };

      return {
        panel_be_render(bid, layer, x, y, w, h, ptr, n) {
          const f = get(bid, layer);
          // A zero-size rect means "not this pass" — the layer did not match. Hide it rather than collapsing it
          // to a dot. Same convention as the button device.
          if (w <= 0 || h <= 0) { f.el.style.display = "none"; return; }
          f.el.style.display = "";
          const s = window.innerHeight / (rt.renderH || 1080);
          // Publish the panel origin + scale so each element host can position itself relative to the panel.
          // ONLY the child-hosting panel (bid 0) may do this — a decorative one would drag the editor across the
          // world with it. (18 = ui.PAD.)
          if (bid === 0) { rt._uiScale = s; rt._uiPanelX = x; rt._uiPanelY = y; }
          f.el.style.left = (x * s) + "px";
          f.el.style.top = (y * s) + "px";
          f.el.style.width = (w * s) + "px";
          f.el.style.height = (h * s) + "px";
          f.el.style.fontSize = (20 * s) + "px";
          const t = self.dec.decode(new Uint8Array(rt.memory().buffer, ptr, n));
          if (f.title) {
            if (f.title.textContent !== t) f.title.textContent = t;
            f.title.style.left = (18 * s) + "px";
            f.title.style.top = (18 * s) + "px";
          }
        },
      };
    },
  });
})();
