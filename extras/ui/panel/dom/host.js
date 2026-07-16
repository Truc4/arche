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

      const get = (bid) => {
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
          // `layout` the native window backend reads), so native + browser lay out identically. Depth (z-index)
          // and pointer-events are set per-frame in panel_be_render from the driver's `z`, not baked here.
          el.style.cssText = "position:absolute;box-sizing:border-box;background:#0b0e14;" +
            "border:1px solid #232838;border-radius:0.6em;box-shadow:0 10px 34px rgba(0,0,0,0.5);overflow:hidden;";
          const t = document.createElement("div");
          t.id = id + "-title";
          t.style.cssText = "position:absolute;font:700 1.15em/1 ui-sans-serif,system-ui,sans-serif;" +
            "letter-spacing:0.08em;color:#cdd6f4;white-space:nowrap;";
          el.appendChild(t);
          const sb = document.createElement("div");
          sb.id = id + "-sub";
          sb.style.cssText = "position:absolute;font:400 0.9em/1 ui-sans-serif,system-ui,sans-serif;" +
            "letter-spacing:0.06em;color:#9298cc;white-space:nowrap;";
          el.appendChild(sb);
          (rt.root || document.body).appendChild(el);
        }
        f = {
          el: el,
          title: document.getElementById(id + "-title"),
          sub: document.getElementById(id + "-sub"),
        };
        self.panels.set(bid, f);
        return f;
      };

      return {
        panel_be_render(bid, z, x, y, w, h, ptr, n) {
          const f = get(bid);
          // Depth is a straight echo of the driver's `z`. Below the foreground canvas's z (SPLIT_Z, published as
          // rt._splitZ by the gfx host) the panel is scenery — behind the world's bodies AND click-through, so a
          // press falls to the world underneath; at/above it the panel is foreground chrome and takes clicks.
          f.el.style.zIndex = z;
          f.el.style.pointerEvents = (z < rt._splitZ) ? "none" : "";
          // A zero-size rect is the DRIVER saying "not now" — hide it. (The button device uses the same rule.)
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
          f.scale = s;
        },
        panel_be_sub(bid, ptr, n) {
          const f = self.panels.get(bid);
          if (!f || !f.sub) return;
          const t = n > 0 ? self.dec.decode(new Uint8Array(rt.memory().buffer, ptr, n)) : "";
          if (f.sub.textContent !== t) f.sub.textContent = t;
          const s = f.scale || 1;
          // Under the title RULE (PAD + TITLE_H), so it reads as a caption on the panel rather than a second
          // title. The framebuffer backend places it at exactly the same offset.
          f.sub.style.left = (18 * s) + "px";
          f.sub.style.top = ((18 + 34 + 12) * s) + "px";
        },
      };
    },
  });
})();
