// Browser host for the `text` device's dom backend — SHIPS WITH THE DEVICE (co-located with backend.arche);
// `arche build --arch=wasm32` collects it into <out>.hosts.js. The arche program owns no glyph pixels — it
// issues `text.draw(x, y, "…", size, color)` calls that lower to the `text_be_draw` / `text_be_clear` wasm
// imports THIS host fulfils by creating absolutely-positioned <span>s in a full-viewport overlay above the
// gfx <canvas>: the browser's own font engine renders crisp, selectable text while gfx keeps the framebuffer
// — canvas pixels below, DOM text above. The generic runtime/arche-web.js supplies WASI + drives the reactor.
//
// COORDINATES: arche gives (x, y, size) in gfx RENDER pixels (0..renderH fixed). The gfx canvas is CSS-sized
// 100vw×100vh with backing aspect = window aspect, so the on-screen scale is uniform `innerHeight / renderH`.
// renderH == the gfx canvas backing height (gfx sets `canvas.height = renderH`), read lazily so the two dom
// hosts stay decoupled. Each run's render-space record is re-placed on resize so labels track the canvas.
(function () {
  (globalThis.archeHosts ??= []).push({
    bind(rt) {
      this.layer = null;   // the overlay div, created lazily on first draw
      this.spans = [];     // POOLED { x, y, size, text, el } in RENDER space; reused across frames
      this._cursor = 0;    // per-frame reuse cursor
      this._dec = new TextDecoder();
      this._onResize = null;
    },

    seams(rt) {
      const self = this;

      // renderH = the gfx canvas backing height (gfx sets canvas.height = renderH); 1 until a canvas exists.
      const scale = () => {
        const c = document.querySelector && document.querySelector("canvas");
        const rh = c && c.height;
        return rh ? window.innerHeight / rh : 1;
      };

      const ensureLayer = () => {
        if (self.layer) return;
        const d = document.getElementById("text-layer") || document.createElement("div");
        d.id = "text-layer";
        // A viewport-anchored positioning context (`inset:0` gives the spans room to lay a line out).
        d.style.cssText = "position:fixed;inset:0;pointer-events:none;";
        if (!d.parentNode) document.body.appendChild(d);
        self.layer = d;
        if (typeof addEventListener === "function" && !self._onResize) {
          self._onResize = () => { for (const rec of self.spans) place(rec); };
          addEventListener("resize", self._onResize);
        }
      };

      const place = (rec) => {
        const s = scale();
        // CENTRE-anchor. The driver hands `x` already offset to the LEFT edge by half the BITMAP width
        // (`tx = centre - len*size/2`), which centres perfectly on the framebuffer backend. But a browser font
        // renders a DIFFERENT (proportional) width, so left-anchoring here left every run off-centre by a
        // per-string amount. Recover the true centre (`x + len*size/2`) and pin the span to it with
        // translateX(-50%), so DOM text centres exactly like the framebuffer regardless of the rendered width.
        rec.el.style.left = rec.cx * s + "px";
        rec.el.style.top = rec.y * s + "px";
        rec.el.style.fontSize = rec.size * s + "px";
      };

      return {
        // text_be_draw(x, y, sPtr, n, size, color, z): a []char lowers to (ptr, len) = (sPtr, n). REUSE the
        // pooled span at the cursor, update in place, advance. Rewriting textContent only on change keeps a
        // text selection alive across the per-frame redraw. Decode from wasm memory each call (it can grow and
        // detach its ArrayBuffer). color is 0xRRGGBB. `z` is the driver's depth ordinal: the overlay div is
        // position:fixed and so is its own stacking context, so the div carries z (its runs share one depth)
        // to place scenery text in the ROOT stack — above the background canvas, below the panels above it.
        text_be_draw(x, y, sPtr, n, size, color, z) {
          ensureLayer();
          self.layer.style.zIndex = z;
          const str = self._dec.decode(new Uint8Array(rt.memory().buffer, sPtr, n));
          let rec = self.spans[self._cursor];
          if (!rec) {
            const el = document.createElement("span");
            // Absolutely positioned; the layer is pointer-events:none so empty areas pass through to the canvas,
            // but each span opts back IN so the text is highlightable/selectable (whitespace:pre keeps spaces).
            el.style.cssText = "position:absolute;pointer-events:auto;user-select:text;-webkit-user-select:text;cursor:text;white-space:pre;transform:translateX(-50%);";
            self.layer.appendChild(el);
            rec = { x: 0, y: 0, cx: 0, size: 0, text: null, el };
            self.spans[self._cursor] = rec;
          }
          self._cursor++;
          if (rec.el.style.display === "none") rec.el.style.display = "";
          if (rec.text !== str) { rec.el.textContent = str; rec.text = str; }
          rec.el.style.color = "#" + ((color >>> 0) & 0xffffff).toString(16).padStart(6, "0");
          rec.x = x; rec.y = y; rec.size = size; rec.cx = x + (n * size) / 2;
          place(rec);
        },
        // text_be_clear(): BEGIN a frame — keep the pooled nodes (a selection on a reused span survives), hide
        // only the spans the previous frame didn't reuse, and reset the cursor.
        text_be_clear() {
          for (let i = self._cursor; i < self.spans.length; i++) self.spans[i].el.style.display = "none";
          self._cursor = 0;
        },
      };
    },
  });
})();
