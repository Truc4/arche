// Browser host for the `gfx` device's wasm backend — SHIPS WITH THE DEVICE (co-located with backend.arche);
// `arche build --arch=wasm32` collects it into <out>.hosts.js. The wasm/dom twin of the native C shims
// (gfx_x11.c / gfx_wl.c): the arche program renders its world into its own [W*H]int software framebuffer and
// `gfx_be_present` hands that buffer to the GPU — arche owns every pixel, the host owns zero. The generic
// runtime/arche-web.js supplies WASI + drives the reactor (arche_run once, arche_frame per rAF); this host
// only fulfils the six gfx_be_* seams. GPU-composited present: the framebuffer is uploaded straight from
// wasm linear memory as a texture and drawn as one fullscreen quad; a two-line fragment shader swizzles the
// colour on the GPU — no per-pixel JS.
(function () {
  // Vertex shader: a fullscreen triangle-strip quad. v_uv flips V so framebuffer row 0 is the TOP row.
  const VS = `
    attribute vec2 a_pos;
    varying vec2 v_uv;
    void main() {
      v_uv = vec2((a_pos.x + 1.0) * 0.5, (1.0 - a_pos.y) * 0.5);
      gl_Position = vec4(a_pos, 0.0, 1.0);
    }`;

  // Fragment shader: the framebuffer int 0x00RRGGBB is bytes [B,G,R,0] in little-endian wasm memory, so the
  // texel arrives as (B,G,R,0). Reorder to real RGB, force opaque alpha. (Getting this wrong swaps red/blue.)
  const FS = `
    precision mediump float;
    varying vec2 v_uv;
    uniform sampler2D u_tex;
    void main() {
      vec4 t = texture2D(u_tex, v_uv);
      gl_FragColor = vec4(t.b, t.g, t.r, 1.0);
    }`;

  function compile(gl, type, src) {
    const s = gl.createShader(type);
    gl.shaderSource(s, src);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
      throw new Error("gfx shader compile failed: " + gl.getShaderInfoLog(s));
    }
    return s;
  }

  // Largest render width we ask the wasm to fill; must match `MAXW` in the arche gfx program's framebuffer.
  const MAXW = 4096;

  (globalThis.archeHosts ??= []).push({
    bind(rt) {
      // The gfx surface is a <canvas>: use rt.root if it is one, else the first canvas under it, else create.
      let c = rt.root && rt.root.tagName === "CANVAS" ? rt.root : null;
      const host = rt.root || document.body;
      if (!c) c = host.querySelector && host.querySelector("canvas");
      if (!c) { c = document.createElement("canvas"); host.appendChild(c); }
      this.canvas = c;
      // preserveDrawingBuffer keeps the last frame readable via gl.readPixels (the e2e reads it); alpha:false
      // + no depth/antialias is the cheapest surface for a 2D blit.
      this.gl = c.getContext("webgl", {
        preserveDrawingBuffer: true, alpha: false, antialias: false, depth: false, stencil: false,
      });
      if (!this.gl) throw new Error("WebGL is not available");
      this.w = 0; this.h = 0; this.renderH = 0;
      this.texW = 0; this.texH = 0;
      this.handle = 1n; // opaque window handle: arche `window` lowers to i64 → crosses as BigInt
      this.tex = null; this.frames = 0;
      this.keys = { left: false, right: false }; // ←/→ (or A/D) held state, read by gfx_be_axis_x
      this.keyQueue = [];                          // discrete presses, drained by gfx_be_key
      // Named-key → code map; MUST match gfx_x11.c's XLookupString bytes + GFX_KEY_* sentinels.
      const NAMED = { Enter: 13, Backspace: 8, Tab: 9, Escape: 27, ArrowLeft: 1000, ArrowRight: 1001, ArrowUp: 1002, ArrowDown: 1003 };
      const set = (down) => (e) => {
        // If a text field is focused (e.g. an embedded editor <textarea>), let it own the keyboard — don't
        // steal a/d/arrows for movement or preventDefault typing. Movement resumes when the canvas/body is focused.
        const ae = document.activeElement;
        if (ae && (ae.tagName === "TEXTAREA" || ae.tagName === "INPUT" || ae.isContentEditable)) return;
        const k = e.key;
        if (k === "ArrowLeft" || k === "a" || k === "A") this.keys.left = down;
        else if (k === "ArrowRight" || k === "d" || k === "D") this.keys.right = down;
        if (down) {
          let code = NAMED[k];
          if (code === undefined && k.length === 1) code = k.charCodeAt(0);
          if (code !== undefined) { this.keyQueue.push(code); e.preventDefault(); }
        }
      };
      if (typeof addEventListener === "function") {
        addEventListener("keydown", set(true));
        addEventListener("keyup", set(false));
      }
      // Pointer state in RENDER px (canvas backing store), read by gfx_be_mouse_*. Mouse events give CSS coords,
      // so scale by the backing/CSS ratio — hit-tests then line up with the framebuffer the app draws into.
      this.mx = 0; this.my = 0; this.mdown = 0;
      const toRender = (e) => {
        const r = c.getBoundingClientRect();
        this.mx = Math.round((e.clientX - r.left) * (c.width / (r.width || 1)));
        this.my = Math.round((e.clientY - r.top) * (c.height / (r.height || 1)));
      };
      if (typeof c.addEventListener === "function") {
        c.addEventListener("mousemove", toRender);
        c.addEventListener("mousedown", (e) => { toRender(e); if (e.button === 0) this.mdown = 1; });
        addEventListener("mouseup", (e) => { if (e.button === 0) this.mdown = 0; });
      }

      // Horizontal scroll accumulator (render px), drained by gfx_be_scroll — fed by the mouse WHEEL and a TOUCH
      // swipe. Positive = scroll the world right. Listeners are on the canvas only, so a wheel/touch over a DOM
      // panel (editor <textarea> / output <pre> / RUN button) scrolls THAT panel, not the world — they self-gate.
      this.scroll = 0;
      const scaleX = () => { const r = c.getBoundingClientRect(); return c.width / (r.width || 1); };
      let lastTouchX = 0;
      if (typeof c.addEventListener === "function") {
        c.addEventListener("wheel", (e) => {
          // A vertical wheel scrolls the horizontal world; ~1 notch (deltaY≈100) → ~200px, matching x11's Button4/5.
          this.scroll += Math.round((e.deltaX || e.deltaY) * 2.0);
          e.preventDefault();
        }, { passive: false });
        c.addEventListener("touchstart", (e) => {
          if (e.touches.length) { const r = c.getBoundingClientRect(); lastTouchX = (e.touches[0].clientX - r.left) * scaleX(); }
        }, { passive: false });
        c.addEventListener("touchmove", (e) => {
          if (e.touches.length) {
            const r = c.getBoundingClientRect();
            const tx = (e.touches[0].clientX - r.left) * scaleX();
            // Direct manipulation: dragging the content left (tx decreasing) moves the camera right (positive).
            this.scroll += Math.round(lastTouchX - tx);
            lastTouchX = tx;
          }
          e.preventDefault();
        }, { passive: false });
      }
      c.dataset.status = "running";
    },

    seams(rt) {
      const self = this;

      // Size the canvas backing store to the window: fixed render height, width = height × window aspect
      // (capped at MAXW). CSS sizes the element to 100vw×100vh; matching backing aspect fills with no bars.
      const sizeToWindow = () => {
        const ch = Math.max(1, window.innerHeight);
        let w = Math.round(self.renderH * window.innerWidth / ch);
        if (w < 1) w = 1;
        if (w > MAXW) w = MAXW;
        if (w === self.w && self.canvas.height === self.renderH) return;
        self.w = w; self.h = self.renderH;
        self.canvas.width = w; self.canvas.height = self.renderH;
      };

      // Build the shader program, fullscreen-quad buffer, and framebuffer texture. Once per open.
      const initGL = (w, h) => {
        const gl = self.gl;
        const prog = gl.createProgram();
        gl.attachShader(prog, compile(gl, gl.VERTEX_SHADER, VS));
        gl.attachShader(prog, compile(gl, gl.FRAGMENT_SHADER, FS));
        gl.linkProgram(prog);
        if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
          throw new Error("gfx program link failed: " + gl.getProgramInfoLog(prog));
        }
        gl.useProgram(prog);
        const quad = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, quad);
        gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
        const loc = gl.getAttribLocation(prog, "a_pos");
        gl.enableVertexAttribArray(loc);
        gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);
        gl.uniform1i(gl.getUniformLocation(prog, "u_tex"), 0);
        self.tex = gl.createTexture();
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, self.tex);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
        gl.viewport(0, 0, w, h);
      };

      // Upload the live w×h region straight from wasm memory and draw it. Recompute the byte view each call:
      // wasm memory can grow and detach its ArrayBuffer, so read rt.memory() lazily. Realloc the texture on
      // resize, else refill in place. No per-pixel JS — the GPU does the swizzle.
      const present = (pxPtr, w, h) => {
        const gl = self.gl;
        const bytes = new Uint8Array(rt.memory().buffer, pxPtr, w * h * 4);
        gl.bindTexture(gl.TEXTURE_2D, self.tex);
        if (w !== self.texW || h !== self.texH) {
          gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, bytes);
          gl.viewport(0, 0, w, h);
          self.texW = w; self.texH = h;
        } else {
          gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, bytes);
        }
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
      };

      if (typeof addEventListener === "function" && !self._onResize) {
        self._onResize = () => sizeToWindow();
        addEventListener("resize", self._onResize);
      }

      return {
        // `h` is the fixed render height; width derives from the window aspect (no bars). title []char ignored.
        gfx_be_open(_w, h, _titlePtr) {
          self.renderH = h;
          rt.renderH = h; // share the render-height scale reference with other hosts (editor/screen place seams)
          sizeToWindow();
          initGL(self.w, self.h);
          return self.handle;
        },
        gfx_be_w() { return self.w; },
        gfx_be_h() { return self.h; },
        gfx_be_present(_win, pxPtr, w, h) {
          present(pxPtr, w, h);
          self.frames++;
          if (self.frames === 1) self.canvas.dataset.status = "live"; // first painted frame (e2e signal)
        },
        gfx_be_poll() { return 1; }, // the tab is always open; native inserts Closed here to exit
        gfx_be_axis_x() { return (self.keys.right ? 1 : 0) - (self.keys.left ? 1 : 0); },
        gfx_be_key() { return self.keyQueue.length ? self.keyQueue.shift() : 0; },
        gfx_be_mouse_x() { return self.mx; },
        gfx_be_mouse_y() { return self.my; },
        gfx_be_mouse_down() { return self.mdown; },
        gfx_be_scroll() { const s = self.scroll; self.scroll = 0; return s; }, // drain-and-clear
        gfx_be_close() {},
      };
    },
  });
})();
