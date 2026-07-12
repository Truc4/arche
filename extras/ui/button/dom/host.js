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
          "color:#160d0a;z-index:6;font:600 1em ui-sans-serif,system-ui,sans-serif;" +
          "transition:transform 50ms linear, box-shadow 50ms linear, background 50ms linear;}" +
          ".arche-btn.is-down{filter:brightness(0.88);}";
        document.head.appendChild(st);
      }
      // ONE REAL <button> PER `bid`. This used to be a hardcoded singleton (getElementById("ui-button") and a
      // single `clicked` flag), so a driver with two Button rows saw them both drive the same element and
      // drain the same flag — the second button silently did not exist.
      this.btns = new Map();
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
        b.addEventListener("pointerdown", (ev) => {
          ev.preventDefault();
          e.clicked = true;
          e.held = true;
          b.classList.add("is-down"); // the pressed look — see the stylesheet
          if (b.setPointerCapture) { try { b.setPointerCapture(ev.pointerId); } catch (_) {} }
        });
        const up = () => { e.held = false; b.classList.remove("is-down"); };
        b.addEventListener("pointerup", up);
        b.addEventListener("pointercancel", up);
        // Holding a movement pad IS a long press, so the browser offers its context menu / text-selection
        // callout right on top of the controls. CSS alone does not stop it (Android fires `contextmenu`
        // regardless of -webkit-touch-callout), so refuse the event outright.
        b.addEventListener("contextmenu", (ev) => { ev.preventDefault(); });
        self.btns.set(bid, e);
        return e;
      };

      return {
        button_be_label(bid, ptr, n, x, y, w, h) {
          const e = get(bid);
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
