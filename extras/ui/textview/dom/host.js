(function () {
  (globalThis.archeHosts ??= []).push({
    bind(rt) {
      this.dec = new TextDecoder();
      // ONE REAL <pre> PER `bid`. This used to be a hardcoded singleton (getElementById("ui-textview")), so a
      // driver with two textview rows saw the second silently overwrite the first's content and rect. Keyed by
      // `bid` (like the panel host), independent textviews work — the portfolio's playground output (bid 0) plus
      // any number of info-card bodies.
      this.views = new Map();
    },
    seams(rt) {
      const self = this;

      const get = (bid) => {
        let el = self.views.get(bid);
        if (el) return el;
        // bid 0 keeps the id "ui-textview" for continuity (the playground output).
        const id = bid === 0 ? "ui-textview" : "ui-textview-" + bid;
        el = document.getElementById(id);
        if (!el) {
          el = document.createElement("pre");
          el.id = id;
          el.style.cssText = "position:absolute;box-sizing:border-box;" +
            "margin:0;overflow:auto;background:#11151f;color:#a6e3a1;border:1px solid #232838;border-radius:0.4em;" +
            "padding:0.7em;white-space:pre-wrap;font:0.92em/1.5 ui-monospace,Menlo,monospace;";
          (rt.root || document.body).appendChild(el);
        }
        self.views.set(bid, el);
        return el;
      };

      return {
        textview_be_render(bid, ptr, n, x, y, w, h, z) {
          // The driver's rect is already in SCREEN space, so place it absolutely in the app root. Depth is
          // echoed from the driver's `z`, keeping it above the foreground panel it visually sits in.
          const el = get(bid);
          // A zero-size rect is the DRIVER saying "not now" — hide it (an info card collapsed shut).
          if (w <= 0 || h <= 0) { el.style.display = "none"; return; }
          el.style.display = "";
          const t = self.dec.decode(new Uint8Array(rt.memory().buffer, ptr, n));
          if (el.textContent !== t) el.textContent = t;
          const s = rt._uiScale || window.innerHeight / (rt.renderH || 1080);
          el.style.zIndex = z;
          el.style.left = (x * s) + "px";
          el.style.top = (y * s) + "px";
          el.style.width = (w * s) + "px";
          el.style.height = (h * s) + "px";
        },
      };
    },
  });
})();
