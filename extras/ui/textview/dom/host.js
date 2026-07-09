(function () {
  (globalThis.archeHosts ??= []).push({
    bind(rt) {
      this.dec = new TextDecoder();
      let el = document.getElementById("ui-textview");
      if (!el) {
        el = document.createElement("pre");
        el.id = "ui-textview";
        el.style.cssText = "order:2;flex:0 0 auto;min-height:5em;max-height:12em;width:100%;box-sizing:border-box;" +
          "margin:0;overflow:auto;background:#11151f;color:#a6e3a1;border:1px solid #232838;border-radius:0.4em;" +
          "padding:0.7em;white-space:pre;font:0.92em/1.5 ui-monospace,Menlo,monospace;";
        (rt.root || document.body).appendChild(el);
      }
      this.el = el;
    },
    seams(rt) {
      const self = this;
      return {
        textview_be_render(ptr, n) {
          const f = document.getElementById("ui-panel");
          if (f && self.el.parentNode !== f) f.appendChild(self.el);
          const t = self.dec.decode(new Uint8Array(rt.memory().buffer, ptr, n));
          if (self.el.textContent !== t) self.el.textContent = t;
        },
      };
    },
  });
})();
