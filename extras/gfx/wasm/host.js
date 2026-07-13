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

  // The FOREGROUND surface's shader. Same swizzle, but pure black is TRANSPARENT rather than opaque, so the
  // layer composites over the DOM instead of hiding it. The framebuffer int carries no alpha channel (it is
  // 0x00RRGGBB — the high byte is always 0), so "was this pixel drawn?" has to be keyed off the colour, and
  // black is the sentinel `split` clears to. Nothing in a scene is usually pure #000000; if a driver needs it,
  // one unit off (#010101) is indistinguishable and opaque.
  const FS_KEY = `
    precision mediump float;
    varying vec2 v_uv;
    uniform sampler2D u_tex;
    void main() {
      vec4 t = texture2D(u_tex, v_uv);
      float lit = step(0.5 / 255.0, max(t.r, max(t.g, t.b)));
      gl_FragColor = vec4(t.b * lit, t.g * lit, t.r * lit, lit);
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
      // TWO SURFACES. The background canvas sits under the host's DOM; the foreground one sits over it, and is
      // transparent wherever the driver drew nothing. That sandwich is the whole point — see gfx.arche's
      // `split`. A driver that never calls `split` only ever touches the background one, and the foreground
      // canvas stays empty and invisible.
      //
      // preserveDrawingBuffer keeps the last frame readable via gl.readPixels (the e2e reads it); no
      // depth/antialias is the cheapest surface for a 2D blit.
      const mkSurface = (canvas, alpha) => {
        const gl = canvas.getContext("webgl", {
          preserveDrawingBuffer: true, alpha: alpha, premultipliedAlpha: false,
          antialias: false, depth: false, stencil: false,
        });
        if (!gl) throw new Error("WebGL is not available");
        return { canvas: canvas, gl: gl, tex: null, texW: 0, texH: 0, alpha: alpha };
      };
      this.bg = mkSurface(c, false);
      // The foreground canvas mirrors the background one's geometry exactly and never takes input — pointer
      // events must fall THROUGH it to the DOM and the background canvas beneath, or it would swallow every
      // click in the world.
      //
      // z-index 3 leaves room for the host's DOM to stack UNDERNEATH it in a deliberate order (a driver's
      // scenery text at 1, its background panels at 2), which is the whole point of the split: those layers
      // must be occluded by the world's foreground, and DOM over a single canvas never can be.
      let fc = document.getElementById("gfx-fg");
      if (!fc) {
        fc = document.createElement("canvas");
        fc.id = "gfx-fg";
        fc.style.cssText = "position:absolute;left:0;top:0;width:100%;height:100%;pointer-events:none;z-index:3;";
        (c.parentNode || host).appendChild(fc);
      }
      this.fg = mkSurface(fc, true);
      if (c.style) { c.style.position = c.style.position || "absolute"; c.style.zIndex = "0"; }
      this.cur = this.bg; // the surface the next present lands on; `split` flips it
      this.w = 0; this.h = 0; this.renderH = 0;
      this.handle = 1n; // opaque window handle: arche `window` lowers to i64 → crosses as BigInt
      this.frames = 0;
      this.keys = { left: false, right: false, up: false, down: false }; // held state, read by gfx_be_axis_x/y
      this.keyQueue = [];                          // discrete presses, drained by gfx_be_key
      // Named-key → code map; MUST match gfx_x11.c's XLookupString bytes + GFX_KEY_* sentinels.
      const NAMED = { Enter: 13, Backspace: 8, Tab: 9, Escape: 27, ArrowLeft: 1000, ArrowRight: 1001, ArrowUp: 1002, ArrowDown: 1003 };
      // Does a foreign TEXT FIELD own the keyboard? (An embedded editor <textarea>, say.) Exposed to the
      // driver as gfx_be_text_focus so it can hold a real focus flag — see gfx.arche's `text_focus`.
      this.textFocused = () => {
        const ae = document.activeElement;
        return !!(ae && (ae.tagName === "TEXTAREA" || ae.tagName === "INPUT" || ae.isContentEditable));
      };
      const set = (down) => (e) => {
        // A focused text field OWNS the keyboard: never steal a/d/arrows for movement, never preventDefault
        // its typing. But the KEYUP must still clear the held axis. The guard used to early-return on keyup
        // too, so holding → and then clicking into the <textarea> mid-hold swallowed the keyup and left
        // keys.right stuck true — axis_x returned +1 forever and the player walked away with no key held.
        // Releases are always safe to observe; only presses are the text field's to keep.
        if (this.textFocused()) {
          if (down) return;
          this.keys.left = false; this.keys.right = false; this.keys.up = false; this.keys.down = false;
          return;
        }
        const k = e.key;
        if (k === "ArrowLeft") this.keys.left = down;
        else if (k === "ArrowRight") this.keys.right = down;
        else if (k === "ArrowUp" || k === " ") this.keys.up = down;
        else if (k === "ArrowDown") this.keys.down = down;
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
      // A Touch has clientX/clientY just like a MouseEvent, so the same conversion serves both.
      const toRenderTouch = (t) => toRender(t);
      if (typeof c.addEventListener === "function") {
        c.addEventListener("mousemove", toRender);
        c.addEventListener("mousedown", (e) => { toRender(e); if (e.button === 0) this.mdown = 1; });
        addEventListener("mouseup", (e) => { if (e.button === 0) this.mdown = 0; });
      }
      // Touch releases anywhere end the press — a finger can leave the canvas before lifting.
      if (typeof addEventListener === "function") {
        addEventListener("touchend", () => { this.mdown = 0; }, { passive: true });
        addEventListener("touchcancel", () => { this.mdown = 0; }, { passive: true });
      }

      // Horizontal scroll accumulator (render px), drained by gfx_be_scroll — fed by the mouse WHEEL and a TOUCH
      // swipe. Positive = scroll the world right. Listeners are on the canvas only, so a wheel/touch over a DOM
      // panel (editor <textarea> / output <pre> / RUN button) scrolls THAT panel, not the world — they self-gate.
      this.scroll = 0;
      const scaleX = () => { const r = c.getBoundingClientRect(); return c.width / (r.width || 1); };
      const nowMs = () => (typeof performance !== "undefined" && performance.now ? performance.now() : Date.now());
      // Momentum model = iOS UIScrollView: velocity in px/ms decays EXPONENTIALLY, v *= DECEL^dt each frame. The
      // release velocity is the ACTUAL finger speed at lift-off (measured over the last ~80ms of samples), so a
      // gentle release glides little even after a fast swipe. DECEL is the feel knob (0.998 iOS "normal", 0.99
      // "fast" = snappier/shorter). Only that + MIN_VEL/MAX_VEL matter.
      const DECEL = 0.998, MIN_VEL = 0.02, MAX_VEL = 6; // per-ms decay; settle/clamp thresholds in px/ms
      let lastTouchX = 0;
      let samples = [];       // recent { x: render-px, t: ms } for the release-velocity estimate
      let flingVel = 0;       // px/ms, decaying during the glide
      let flingFrac = 0, flingLast = 0;
      let momentumRAF = null;
      const stopMomentum = () => { if (momentumRAF !== null) { cancelAnimationFrame(momentumRAF); momentumRAF = null; } flingVel = 0; };
      if (typeof c.addEventListener === "function") {
        c.addEventListener("wheel", (e) => {
          // A vertical wheel scrolls the horizontal world; ~1 notch (deltaY≈100) → ~200px, matching x11's Button4/5.
          this.scroll += Math.round((e.deltaX || e.deltaY) * 2.0);
          e.preventDefault();
        }, { passive: false });
        c.addEventListener("touchstart", (e) => {
          stopMomentum(); // a fresh touch grabs the world — kill any in-flight fling
          if (e.touches.length) { const r = c.getBoundingClientRect(); lastTouchX = (e.touches[0].clientX - r.left) * scaleX(); samples = [{ x: lastTouchX, t: nowMs() }]; }
          // A touch on the canvas is a POINTER PRESS on the world, not just a scroll gesture. Without this,
          // `mdown` is only ever written by mouse events, so on a phone nothing can observe a press on the
          // world: a driver could never take keyboard focus BACK from an embedded <textarea> (it has no click
          // edge to react to) and the player would stay dead forever. The scroll/fling accumulation above is
          // untouched — a swipe both pans the world AND counts as touching it, which is what you want.
          if (e.touches.length) { toRenderTouch(e.touches[0]); this.mdown = 1; }
        }, { passive: false });
        c.addEventListener("touchmove", (e) => {
          if (e.touches.length) {
            const r = c.getBoundingClientRect();
            const tx = (e.touches[0].clientX - r.left) * scaleX();
            // Direct manipulation: dragging the content left (tx decreasing) moves the camera right (positive).
            this.scroll += Math.round(lastTouchX - tx);
            lastTouchX = tx;
            const t = nowMs();
            samples.push({ x: tx, t });
            while (samples.length > 2 && t - samples[0].t > 80) samples.shift(); // keep only the last ~80ms
            toRenderTouch(e.touches[0]); // keep the pointer position live under a dragging finger
          }
          e.preventDefault();
        }, { passive: false });
        c.addEventListener("touchend", () => {
          // Release velocity = displacement / time over the last ~80ms (a pause before lifting ⇒ ~0 ⇒ no fling).
          if (samples.length >= 2) {
            const a = samples[0], b = samples[samples.length - 1], dt = b.t - a.t;
            if (dt > 0) flingVel = (a.x - b.x) / dt; // finger moved left → content-left → +scroll
          }
          samples = [];
          if (Math.abs(flingVel) > MAX_VEL) flingVel = flingVel < 0 ? -MAX_VEL : MAX_VEL;
          if (Math.abs(flingVel) < MIN_VEL || typeof requestAnimationFrame !== "function") { flingVel = 0; return; }
          flingFrac = 0; flingLast = nowMs();
          const step = () => {
            const t = nowMs(), dt = Math.min(64, t - flingLast); // clamp long gaps (e.g. tab was backgrounded)
            flingLast = t;
            flingVel *= Math.pow(DECEL, dt);
            if (Math.abs(flingVel) < MIN_VEL) { momentumRAF = null; flingVel = 0; return; }
            flingFrac += flingVel * dt;
            const w = Math.trunc(flingFrac);
            this.scroll += w; flingFrac -= w;
            momentumRAF = requestAnimationFrame(step);
          };
          momentumRAF = requestAnimationFrame(step);
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
        if (w === self.w && self.bg.canvas.height === self.renderH) return;
        self.w = w; self.h = self.renderH;
        // Both surfaces must stay identical, or the two halves of the frame would not line up.
        for (const su of [self.bg, self.fg]) { su.canvas.width = w; su.canvas.height = self.renderH; }
      };

      // Build the shader program, fullscreen-quad buffer, and framebuffer texture. Once per open.
      const initGL = (su, w, h) => {
        const gl = su.gl;
        const prog = gl.createProgram();
        gl.attachShader(prog, compile(gl, gl.VERTEX_SHADER, VS));
        gl.attachShader(prog, compile(gl, gl.FRAGMENT_SHADER, su.alpha ? FS_KEY : FS));
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
        su.tex = gl.createTexture();
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, su.tex);
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
      const present = (su, pxPtr, w, h) => {
        const gl = su.gl;
        const bytes = new Uint8Array(rt.memory().buffer, pxPtr, w * h * 4);
        gl.bindTexture(gl.TEXTURE_2D, su.tex);
        if (w !== su.texW || h !== su.texH) {
          gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, bytes);
          gl.viewport(0, 0, w, h);
          su.texW = w; su.texH = h;
        } else {
          gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, bytes);
        }
        if (su.alpha) { gl.clearColor(0, 0, 0, 0); gl.clear(gl.COLOR_BUFFER_BIT); }
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
          initGL(self.bg, self.w, self.h);
          initGL(self.fg, self.w, self.h);
          return self.handle;
        },
        gfx_be_w() { return self.w; },
        gfx_be_h() { return self.h; },
        gfx_be_present(_win, pxPtr, w, h) {
          present(self.cur, pxPtr, w, h);
          self.cur = self.bg; // next frame starts on the background surface again
          self.frames++;
          if (self.frames === 1) self.canvas.dataset.status = "live"; // first painted frame (e2e signal)
        },
        gfx_be_poll() { return 1; }, // the tab is always open; native inserts Closed here to exit
        gfx_be_axis_x() { return (self.keys.right ? 1 : 0) - (self.keys.left ? 1 : 0); },
        gfx_be_axis_y() { return (self.keys.down ? 1 : 0) - (self.keys.up ? 1 : 0); },
        gfx_be_key() { return self.keyQueue.length ? self.keyQueue.shift() : 0; },
        gfx_be_mouse_x() { return self.mx; },
        gfx_be_mouse_y() { return self.my; },
        gfx_be_mouse_down() { return self.mdown; },
        gfx_be_text_focus() { return self.textFocused() ? 1 : 0; },
        // Is this a TOUCH device? `pointer: coarse` is the standard test — it asks about the primary input's
        // precision, not the screen size, so a narrow desktop window stays "fine" and a landscape phone stays
        // "coarse". A driver uses it to show on-screen controls; native backends report 0 (they have a mouse).
        // Everything so far belongs to the BACKGROUND: present it, then wipe the framebuffer to the
        // transparent sentinel (black) so the rest of the frame composites over the DOM rather than hiding it.
        // The zeroing is the only per-pixel work the host does, and it is a plain fill.
        gfx_be_split(_win, pxPtr, w, h) {
          present(self.bg, pxPtr, w, h);
          new Uint8Array(rt.memory().buffer, pxPtr, w * h * 4).fill(0);
          self.cur = self.fg;
        },
        gfx_be_coarse_pointer() {
          if (typeof matchMedia !== "function") return 0;
          return matchMedia("(pointer: coarse)").matches ? 1 : 0;
        },
        // Take the keyboard back for the world: blur the focused text field and focus the canvas. Also clear
        // the held axis — the blur means we will never see the keyup for anything currently held.
        gfx_be_release_text() {
          if (!self.textFocused()) return 0;
          document.activeElement.blur();
          if (self.canvas.focus) self.canvas.focus();
          self.keys.left = false; self.keys.right = false;
          return 1;
        },
        gfx_be_scroll() { const s = self.scroll; self.scroll = 0; return s; }, // drain-and-clear
        gfx_be_close() {},
      };
    },
  });
})();
