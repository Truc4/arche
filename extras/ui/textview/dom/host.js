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
          const f = document.getElementById("ui-panel");
          if (f && self.el.parentNode !== f) f.appendChild(self.el);
          const t = self.dec.decode(new Uint8Array(rt.memory().buffer, ptr, n));
          if (self.el.textContent !== t) self.el.textContent = t;
          const s = rt._uiScale || window.innerHeight / (rt.renderH || 1080);
          self.el.style.left = ((x - (rt._uiPanelX || 0)) * s) + "px";
          self.el.style.top = ((y - (rt._uiPanelY || 0)) * s) + "px";
          self.el.style.width = (w * s) + "px";
          self.el.style.height = (h * s) + "px";
        },
      };
    },
  });
})();
