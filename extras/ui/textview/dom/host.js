(function () {
  (globalThis.archeHosts ??= []).push({
    bind(rt) {
      this.dec = new TextDecoder();
      let el = document.getElementById("ui-textview");
      if (!el) {
        el = document.createElement("pre");
        el.id = "ui-textview";
        el.style.cssText = "position:absolute;box-sizing:border-box;" +
          "margin:0;overflow:auto;background:#11151f;color:#a6e3a1;border:1px solid #232838;border-radius:0.4em;" +
          "padding:0.7em;white-space:pre;font:0.92em/1.5 ui-monospace,Menlo,monospace;";
        (rt.root || document.body).appendChild(el);
      }
      this.el = el;
    },
    seams(rt) {
      const self = this;
      return {
        textview_be_render(ptr, n, x, y, w, h) {
          // The driver's rect is already in SCREEN space, so place it absolutely in the app root — do NOT reparent
          // into the panel div and subtract the panel's origin. That coupling made the element depend on the panel
          // existing FIRST, and the panel is now created lazily on its first render, which happens after `open`
          // runs at boot: the element never found it, stayed a child of the root, and had panel-relative
          // coordinates applied absolutely — so it sat glued to the screen instead of scrolling with the world.
          // z-index 6 keeps it above the foreground panel (5) it visually sits in.
          const t = self.dec.decode(new Uint8Array(rt.memory().buffer, ptr, n));
          if (self.el.textContent !== t) self.el.textContent = t;
          const s = rt._uiScale || window.innerHeight / (rt.renderH || 1080);
          self.el.style.zIndex = "6";
          self.el.style.left = (x * s) + "px";
          self.el.style.top = (y * s) + "px";
          self.el.style.width = (w * s) + "px";
          self.el.style.height = (h * s) + "px";
        },
      };
    },
  });
})();
