// Browser host for the `embed` device's dom backend — SHIPS WITH THE DEVICE (co-located with backend.arche);
// `arche build --arch=wasm32` collects it into <out>.hosts.js. The arche program owns no browser — it issues
// `embed_be_*` calls that lower to wasm imports THIS host fulfils by reconciling ONE real element per `bid`,
// absolutely positioned over the gfx <canvas>: an <iframe> for a LIVE site (kind 0), or an <img> in a plain
// <div> for a SHOT (kind 1).
//
// COORDINATES: identical to textview/dom — the driver's rect is already in RENDER pixels, so scale by
// `rt._uiScale || innerHeight / renderH` and place absolutely. z-index 6 keeps a card above the foreground
// panel (5) it visually sits in.
//
// A card is NOT a link, and it owns no controls. Opening the real site is a `button` — a driver-owned Button
// row that the button device renders and reports `clicked` for, exactly like the playground's RUN. The driver
// turns that click into `eopen`, and `embed_be_open` below is the only thing here that knows what a URL is.
//
// LAZY: the driver's cards are always open, so "has a non-zero rect" does NOT mean "the visitor is looking at
// it" — every card has one from the first frame. What still separates them is WHERE that rect is: a card the
// visitor has not walked to sits far off the side of the viewport. So `src` is assigned the first time a card
// comes within a screen-width of the viewport, and cached forever after. Without this, every embedded site
// downloads at page load — sites the visitor may never walk to. Once loaded a card is never re-`src`ed, so
// walking past twice does not re-fetch (and does not lose the site's scroll position).
//
// FOCUS: clicking into a cross-origin iframe moves focus out of this document, and the arrow keys stop reaching
// the world — the same contract the playground's editor already has ("click the world to resume walking"). The
// browser will not blur a cross-origin iframe for us, so a pointerdown ANYWHERE outside one explicitly does.
(function () {
  (globalThis.archeHosts ??= []).push({
    bind(rt) {
      this.dec = new TextDecoder();
      // ONE REAL ELEMENT PER `bid` — keyed like the panel/textview hosts, so N cards are independent.
      this.cards = new Map();
      this._blurWired = false;
    },

    seams(rt) {
      const self = this;

      // A pointerdown outside the focused card hands the keyboard — and the camera — back to the world (see
      // FOCUS above). This must release ANY focused card, not just an iframe: a shot is focusable too, and if
      // it kept focus the driver would keep re-centring the camera on it every frame and never let go.
      const wireBlur = () => {
        if (self._blurWired || typeof addEventListener !== "function") return;
        self._blurWired = true;
        addEventListener("pointerdown", (e) => {
          const a = document.activeElement;
          if (!a) return;
          let owner = null;
          for (const c of self.cards.values()) if (a === c.el || c.el.contains(a)) owner = c;
          if (!owner) return;                                             // focus isn't on a card; not ours
          if (owner.el === e.target || owner.el.contains(e.target)) return;   // clicked the card it's on
          a.blur();
          window.focus();
        }, true);
      };

      const get = (bid, kind) => {
        let c = self.cards.get(bid);
        if (c) return c;
        const id = "ui-embed-" + bid;
        let el = document.getElementById(id);
        let inner = null;
        if (!el) {
          const frame = "position:absolute;box-sizing:border-box;margin:0;overflow:hidden;" +
            "background:#11151f;border:1px solid #232838;border-radius:0.4em;";
          if (kind === 1) {
            // SHOT: a still picture of the site. Just a picture — it goes nowhere; the card's OPEN button does
            // that, the same way it does for a live one.
            //
            // `tabindex` is what makes it FOCUSABLE, and focus is how a card tells the driver it was clicked
            // (see `embed_be_focused`). A <div> takes no focus by default, so without this a shot could never
            // bring the camera to itself the way a live embed does — clicking one would do nothing at all.
            // No focus ring: the camera moving to the card is the feedback.
            el = document.createElement("div");
            el.tabIndex = 0;
            el.style.cssText = frame + "display:block;outline:none;cursor:pointer;";
            inner = document.createElement("img");
            inner.style.cssText = "display:block;width:100%;height:100%;object-fit:cover;object-position:top center;";
            el.appendChild(inner);
          } else {
            // LIVE: the site itself. `no-referrer` keeps this page out of the embedded site's logs.
            el = document.createElement("iframe");
            el.setAttribute("loading", "lazy");
            el.setAttribute("referrerpolicy", "no-referrer");
            el.style.cssText = frame + "border-width:1px;display:block;";
          }
          el.id = id;
          (rt.root || document.body).appendChild(el);
        }
        wireBlur();
        c = { el, inner, kind, src: null };
        self.cards.set(bid, c);
        return c;
      };

      return {
        embed_be_render(bid, sPtr, n, kind, x, y, w, h) {
          const c = get(bid, kind);
          // A zero-size rect is the DRIVER saying "not now" — hide it. Don't touch `src`: hiding must not
          // unload a site the visitor already opened.
          if (w <= 0 || h <= 0) { c.el.style.display = "none"; return; }
          c.el.style.display = "";

          const s = rt._uiScale || window.innerHeight / (rt.renderH || 1080);
          const px = x * s, py = y * s, pw = w * s, ph = h * s;
          c.el.style.zIndex = "6";
          c.el.style.left = px + "px";
          c.el.style.top = py + "px";
          c.el.style.width = pw + "px";
          c.el.style.height = ph + "px";
          c.el.style.pointerEvents = "auto";

          // See LAZY above: fetch once the card is within one screen-width of the viewport.
          if (c.src === null) {
            const m = window.innerWidth;
            const near = px < window.innerWidth + m && px + pw > -m &&
                         py < window.innerHeight + m && py + ph > -m;
            if (!near) return;
            c.src = self.dec.decode(new Uint8Array(rt.memory().buffer, sPtr, n));
            if (c.kind === 1) c.inner.src = c.src;
            else c.el.src = c.src;
          }
        },

        // Which card holds focus? The browser focuses whatever element was clicked, including a cross-origin
        // iframe, so `document.activeElement` IS the answer — no click plumbing of our own. 1/0 per bid.
        embed_be_focused(bid) {
          const c = self.cards.get(bid);
          if (!c) return 0;
          const a = document.activeElement;
          return a && (a === c.el || c.el.contains(a)) ? 1 : 0;
        },

        // The driver says "open this one" (its OPEN button was clicked this frame) and hands over the URL.
        // `noopener` — the target is a plain external site and must not get a handle on this window.
        embed_be_open(lPtr, n) {
          if (n <= 0) return;
          const url = self.dec.decode(new Uint8Array(rt.memory().buffer, lPtr, n));
          window.open(url, "_blank", "noopener,noreferrer");
        },
      };
    },
  });
})();
