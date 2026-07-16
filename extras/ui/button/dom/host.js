(function () {
  (globalThis.archeHosts ??= []).push({
    bind(rt) {
      this.dec = new TextDecoder();
      // Default skin, injected once. A page can restyle any button by id (`#ui-button-<bid>`) because nothing
      // here is inline. `.is-down` is the PRESSED state, toggled on pointerdown/up — a button you can HOLD has
      // to look held, and CSS is the only place that animation belongs.
      if (!document.getElementById("arche-btn-css")) {
        const st = document.createElement("style");
        st.id = "arche-btn-css";
        st.textContent =
          // -webkit-touch-callout:none kills the iOS long-press callout. A button you HOLD is pressed for
          // seconds at a time, which is exactly the gesture the OS reads as "select / share this element".
          ".arche-btn{cursor:pointer;touch-action:none;user-select:none;-webkit-user-select:none;" +
          "-webkit-touch-callout:none;" +
          "-webkit-tap-highlight-color:transparent;border:none;border-radius:0.4em;background:#e4694e;" +
          "color:#160d0a;font:600 1em ui-sans-serif,system-ui,sans-serif;" +
          "transition:transform 50ms linear, box-shadow 50ms linear, background 50ms linear;}" +
          ".arche-btn.is-down{filter:brightness(0.88);}";
        document.head.appendChild(st);
      }
      // ONE REAL <button> PER `bid`. This used to be a hardcoded singleton (getElementById("ui-button") and a
      // single `clicked` flag), so a driver with two Button rows saw them both drive the same element and
      // drain the same flag — the second button silently did not exist.
      this.btns = new Map();
      // Pointers currently down ANYWHERE. A button only knows it was entered, not whether a finger is actually
      // pressed — this is what tells `pointerenter` the difference between a slide-in and a hover.
      this.down = new Set();
      const lift = (ev) => {
        this.down.delete(ev.pointerId);
        // Last finger up ⇒ nothing can be held. A release that lands outside every button (lifting off the
        // edge of one) would otherwise never reach a `pointerup` handler, and the pad would stay stuck down.
        if (!this.down.size) { this.btns.forEach((e) => e.release && e.release()); }
      };
      if (typeof addEventListener === "function") {
        addEventListener("pointerup", lift);
        addEventListener("pointercancel", lift);
      }
    },
    seams(rt) {
      const self = this;

      const get = (bid) => {
        let e = self.btns.get(bid);
        if (e) return e;
        const b = document.createElement("button");
        b.id = "ui-button-" + bid;
        b.type = "button";
        b.className = "arche-btn";
        // Only LAYOUT is inline (the driver owns the rect). APPEARANCE lives in the injected stylesheet below,
        // keyed off `.arche-btn` — inline styles beat author CSS, so baking the look in here would make the
        // button unskinnable by the page embedding it.
        // Depth is echoed from the driver's `z` per-frame in button_be_label, straddling the two gfx canvases
        // exactly as a panel's does: a button below the foreground canvas's z sits UNDER it, so the player walks
        // in front, while one above floats over everything. A background button still takes clicks — the
        // foreground canvas is pointer-events:none, so the press falls straight through to it.
        b.style.cssText = "position:absolute;box-sizing:border-box;";
        // Appended to the app root, NOT into #ui-panel. The old host reparented into the panel div (which is
        // overflow:hidden) and positioned relative to it, so a button anchored anywhere else on the viewport —
        // an on-screen movement pad, say — was clipped out of existence. The driver's rect is already in the
        // same screen space the panel's own rect came from, so absolute placement lands identically.
        (rt.root || document.body).appendChild(b);
        e = { el: b, clicked: false, held: false };
        // `pointer*` covers mouse AND touch in one path, and setPointerCapture keeps the press ours even if the
        // finger slides off the pad — without it, `pointerup` would fire on some other element and the pad
        // would stay stuck down, walking the player forever.
        const press = () => { e.clicked = true; e.held = true; b.classList.add("is-down"); };
        const release = () => { e.held = false; b.classList.remove("is-down"); };
        e.release = release;
        b.addEventListener("pointerdown", (ev) => {
          ev.preventDefault();
          // RELEASE the implicit capture. A touch pointer is captured to whatever element got `pointerdown`,
          // so every later event — including the ones over OTHER buttons — is delivered here. That makes it
          // impossible to slide a thumb from LEFT onto RIGHT: the second button never hears a thing. Letting
          // the capture go restores the boundary events (pointerenter/pointerleave) that make sliding work.
          if (b.releasePointerCapture && b.hasPointerCapture && b.hasPointerCapture(ev.pointerId)) {
            try { b.releasePointerCapture(ev.pointerId); } catch (_) {}
          }
          self.down.add(ev.pointerId);
          press();
        });
        // Sliding: a finger already down that crosses into this button presses it, and leaving un-presses it.
        b.addEventListener("pointerenter", () => { if (self.down.size) press(); });
        b.addEventListener("pointerleave", release);
        b.addEventListener("pointerup", release);
        b.addEventListener("pointercancel", release);
        // Holding a movement pad IS a long press, so the browser wants to offer its context menu / selection
        // callout — with a haptic buzz — right on top of the controls.
        //
        // Cancelling `contextmenu` is NOT enough: on Android the long-press haptic fires BEFORE that event, so
        // you still feel the buzz even though no menu appears. The GESTURE itself has to be refused, and the
        // only thing that does that is preventDefault on `touchstart` (which must be a non-passive listener,
        // or the browser ignores it). preventDefault on `pointerdown` does not cover it.
        b.addEventListener("touchstart", (ev) => { ev.preventDefault(); }, { passive: false });
        b.addEventListener("contextmenu", (ev) => { ev.preventDefault(); });
        self.btns.set(bid, e);
        return e;
      };

      return {
        button_be_label(bid, z, ptr, n, x, y, w, h) {
          const e = get(bid);
          e.el.style.zIndex = z;
          const t = self.dec.decode(new Uint8Array(rt.memory().buffer, ptr, n));
          if (e.el.textContent !== t) e.el.textContent = t;
          // A ZERO-SIZED button is the driver saying "not now" (e.g. touch controls on a desktop). Hide it
          // outright, so it cannot swallow clicks meant for the world behind it.
          if (w <= 0 || h <= 0) { e.el.style.display = "none"; return; }
          e.el.style.display = "";
          const s = rt._uiScale || window.innerHeight / (rt.renderH || 1080);
          e.el.style.left = (x * s) + "px";
          e.el.style.top = (y * s) + "px";
          e.el.style.width = (w * s) + "px";
          e.el.style.height = (h * s) + "px";
        },
        // `clicked` DRAINS (it is an edge, consumed once); `held` does not (it is a level, sampled).
        button_be_clicked(bid) { const e = get(bid); const c = e.clicked; e.clicked = false; return c ? 1 : 0; },
        button_be_held(bid) { return get(bid).held ? 1 : 0; },
      };
    },
  });
})();
