/* X11 (Xlib) backend shim for the arche `gfx` device. Auto-discovered + compiled because it sits in
 * the gfx/x11/ variant folder; libX11 is supplied by x11/backend.arche's `#link { "X11" }`.
 *
 * Resizable software framebuffer: the SHIM owns the pixel buffer (sized to the window, reallocated on
 * resize). arche asks for the current frame — a writable view (pointer + current w/h) — fills it, and
 * calls present(). This is the only way a tiling WM (which forces the window size) shows a full-window
 * image rather than a fixed-size patch in the corner. `window` (opaque on the arche side) is the
 * GfxX11* pointer; pixels are 0xRRGGBB ints, presented inline via XPutImage (no MIT-SHM). */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "gfx_x11.h"

typedef struct {
	Display *dpy;
	Window win;
	GC gc;
	XImage *img;
	int *buf; /* XImage-owned pixel store (freed by XDestroyImage) */
	int w, h;
	Atom wm_delete;
	int open;
	int left, right; /* ←/→ arrow key held state, updated in poll, read by gfx_be_axis_x */
	int up, down;    /* ↑/↓ (+ Space/W/S) held state, read by gfx_be_axis_y */
	/* Discrete key FIFO for gfx_be_key (an editor needs each keypress, not held state). Printable keys are
	 * their ASCII byte; special keys use the sentinels below. */
	int keyq[64];
	int keyq_head, keyq_tail;
	int mx, my, mdown; /* pointer position (window px) + left-button held state, for gfx_be_mouse_* */
	int scroll;        /* wheel accumulator (Button4/Button5), drained by gfx_be_scroll */
} GfxX11;

/* Non-ASCII key sentinels returned by gfx_be_key — MUST match the browser host (gfx.js / wasm/host.js). */
enum { GFX_KEY_LEFT = 1000, GFX_KEY_RIGHT = 1001, GFX_KEY_UP = 1002, GFX_KEY_DOWN = 1003 };

static void keyq_push(GfxX11 *g, int k) {
	int next = (g->keyq_tail + 1) % (int)(sizeof(g->keyq) / sizeof(g->keyq[0]));
	if (next == g->keyq_head)
		return; /* full — drop */
	g->keyq[g->keyq_tail] = k;
	g->keyq_tail = next;
}

/* (Re)allocate the framebuffer + XImage to w x h. No-op if already that size. */
static void ensure_size(GfxX11 *g, int w, int h) {
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	if (g->img && w == g->w && h == g->h)
		return;
	if (g->img)
		XDestroyImage(g->img); /* frees the old g->buf */
	g->w = w;
	g->h = h;
	g->buf = calloc((size_t)w * (size_t)h, 4);
	int screen = DefaultScreen(g->dpy);
	g->img = XCreateImage(g->dpy, DefaultVisual(g->dpy, screen), (unsigned)DefaultDepth(g->dpy, screen), ZPixmap, 0,
	                      (char *)g->buf, (unsigned)w, (unsigned)h, 32, 0);
}

void *gfx_be_open(int w, int h, char *title) {
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy)
		return NULL;
	int screen = DefaultScreen(dpy);
	unsigned long black = BlackPixel(dpy, screen);
	Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0, (unsigned)w, (unsigned)h, 0, black, black);
	XStoreName(dpy, win, title ? title : "arche");
	XClassHint *cls = XAllocClassHint(); /* WM_CLASS so a WM rule can target us, e.g. class "arche-gfx" */
	if (cls) {
		cls->res_name = (char *)"arche-gfx";
		cls->res_class = (char *)"arche-gfx";
		XSetClassHint(dpy, win, cls);
		XFree(cls);
	}
	/* Default FIXED size (min == max): a non-resizable window most tiling WMs auto-float, so the window
	 * floats out of the box with no compositor rule. `ARCHE_GFX_RESIZABLE=1` opts into a resizable window
	 * (the framebuffer reallocs on ConfigureNotify) — but a resizable window tiles unless you add a WM rule
	 * keyed on the WM_CLASS "arche-gfx" above. Matches the Wayland backend. */
	if (!getenv("ARCHE_GFX_RESIZABLE")) {
		XSizeHints *sh = XAllocSizeHints();
		if (sh) {
			sh->flags = PMinSize | PMaxSize;
			sh->min_width = sh->max_width = w;
			sh->min_height = sh->max_height = h;
			XSetWMNormalHints(dpy, win, sh);
			XFree(sh);
		}
	}
	XSelectInput(dpy, win,
	             ExposureMask | KeyPressMask | KeyReleaseMask | StructureNotifyMask | ButtonPressMask |
	                 ButtonReleaseMask | PointerMotionMask);
	Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(dpy, win, &wm_delete, 1);
	XMapWindow(dpy, win);

	GfxX11 *g = calloc(1, sizeof(GfxX11));
	if (!g) {
		XCloseDisplay(dpy);
		return NULL;
	}
	g->dpy = dpy;
	g->win = win;
	g->gc = XCreateGC(dpy, win, 0, NULL);
	g->wm_delete = wm_delete;
	g->open = 1;
	ensure_size(g, w, h);
	return g;
}

int gfx_be_w(void *handle) {
	GfxX11 *g = handle;
	return g ? g->w : 0;
}
int gfx_be_h(void *handle) {
	GfxX11 *g = handle;
	return g ? g->h : 0;
}

/* Present the CALLER's framebuffer (the driver-owned `Framebuffer` pool, `px`): copy it into the XImage-backed
 * store, then blit. `px` is in-out on the arche side (the FFI borrow rule) but read-only here. */
void gfx_be_present(void *handle, int *px, int w, int h) {
	GfxX11 *g = handle;
	if (!g || !g->img || !g->buf || !px)
		return;
	size_t nbytes = (size_t)w * (size_t)h * 4;
	size_t cap = (size_t)g->w * (size_t)g->h * 4;
	if (nbytes > cap)
		nbytes = cap; /* window resized smaller than the fixed pool — clamp, never overrun */
	memcpy(g->buf, px, nbytes);
	XPutImage(g->dpy, g->win, g->gc, g->img, 0, 0, 0, 0, (unsigned)g->w, (unsigned)g->h);
	XFlush(g->dpy);

	/* Frame limiter: cap to ~60 FPS. XPutImage/XFlush do NOT block on vsync, so without this the reactor's
	 * `forever` loop spins as fast as the CPU allows and everything moves absurdly fast. arche gfx programs
	 * step per-frame assuming ~60 Hz (that's what the browser's requestAnimationFrame gives), so pace the
	 * native loop to the same cadence: sleep out the remainder of a 1/60 s budget since the last present. */
	static const long FRAME_NS = 16666667L; /* 1e9 / 60 */
	static struct timespec last = {0, 0};
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (last.tv_sec || last.tv_nsec) {
		long dt = (long)(now.tv_sec - last.tv_sec) * 1000000000L + (now.tv_nsec - last.tv_nsec);
		if (dt >= 0 && dt < FRAME_NS) {
			struct timespec sl = {0, FRAME_NS - dt};
			nanosleep(&sl, NULL);
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &last); /* stamp AFTER the sleep — the frame boundary */
}

int gfx_be_poll(void *handle) {
	GfxX11 *g = handle;
	if (!g)
		return 0;
	while (XPending(g->dpy)) {
		XEvent ev;
		XNextEvent(g->dpy, &ev);
		if (ev.type == ConfigureNotify) {
			/* Window resized (e.g. a tiling WM placed it): grow/shrink the framebuffer to match. */
			ensure_size(g, ev.xconfigure.width, ev.xconfigure.height);
		} else if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == g->wm_delete) {
			g->open = 0;
		} else if (ev.type == KeyPress || ev.type == KeyRelease) {
			/* Track the ←/→ arrows for gfx_be_axis_x AND enqueue discrete presses for gfx_be_key (an editor
			 * needs each keystroke). Auto-repeat sends Release+Press pairs. Escape closes the window. */
			int down = (ev.type == KeyPress);
			char buf[16];
			KeySym ks;
			int n = XLookupString(&ev.xkey, buf, sizeof(buf), &ks, NULL);
			/* HELD movement state for gfx_be_axis_x / gfx_be_axis_y. Arrows + Space only: WASD are PRINTABLE
			 * characters that an editor must still receive, so a movement binding for them has to be tracked
			 * alongside the dispatch below rather than inside it — and the two backends drifted apart doing
			 * exactly that. Space is the one exception, and it is tracked here for the same reason. */
			if (ks == XK_Left)
				g->left = down;
			if (ks == XK_Right)
				g->right = down;
			if (ks == XK_Up || ks == XK_space)
				g->up = down;
			if (ks == XK_Down)
				g->down = down;
			if (ks == XK_Left) {
				if (down)
					keyq_push(g, GFX_KEY_LEFT);
			} else if (ks == XK_Right) {
				if (down)
					keyq_push(g, GFX_KEY_RIGHT);
			} else if (ks == XK_Up) {
				if (down)
					keyq_push(g, GFX_KEY_UP);
			} else if (ks == XK_Down) {
				if (down)
					keyq_push(g, GFX_KEY_DOWN);
			} else if (ks == XK_Escape) {
				if (down)
					g->open = 0;
			} else if (down) {
				/* Printable + control (Enter=13, Backspace=8, Tab=9…) come through XLookupString. */
				for (int i = 0; i < n; i++)
					keyq_push(g, (unsigned char)buf[i]);
			}
		} else if (ev.type == MotionNotify) {
			g->mx = ev.xmotion.x;
			g->my = ev.xmotion.y;
		} else if (ev.type == ButtonPress || ev.type == ButtonRelease) {
			g->mx = ev.xbutton.x;
			g->my = ev.xbutton.y;
			if (ev.xbutton.button == Button1)
				g->mdown = (ev.type == ButtonPress);
			/* X11 delivers wheel scroll as Button4 (up) / Button5 (down) presses; accumulate ~200px per notch. */
			else if (ev.type == ButtonPress && ev.xbutton.button == Button5)
				g->scroll += 200;
			else if (ev.type == ButtonPress && ev.xbutton.button == Button4)
				g->scroll -= 200;
		}
	}
	return g->open;
}

/* Horizontal input axis: +1 while → is held, -1 while ← is held, 0 for neither or both. */
int gfx_be_axis_x(void *handle) {
	GfxX11 *g = handle;
	if (!g)
		return 0;
	return (g->right ? 1 : 0) - (g->left ? 1 : 0);
}

/* The vertical partner to gfx_be_axis_x: -1 while up is held, +1 while down is. HELD, not pressed — see
 * gfx.arche's `axis_y` for why the drained key queue cannot answer this. */
int gfx_be_axis_y(void *handle) {
	GfxX11 *g = handle;
	if (!g)
		return 0;
	return (g->down ? 1 : 0) - (g->up ? 1 : 0);
}

/* Next discrete key press (ASCII byte, or a GFX_KEY_* sentinel), or 0 if the queue is empty. Non-blocking:
 * call once per frame after gfx_be_poll to drain input for an editor/text field. */
int gfx_be_key(void *handle) {
	GfxX11 *g = handle;
	if (!g || g->keyq_head == g->keyq_tail)
		return 0;
	int k = g->keyq[g->keyq_head];
	g->keyq_head = (g->keyq_head + 1) % (int)(sizeof(g->keyq) / sizeof(g->keyq[0]));
	return k;
}

/* Pointer position (window pixels) + left-button held state, all updated in gfx_be_poll. */
int gfx_be_mouse_x(void *handle) {
	GfxX11 *g = handle;
	return g ? g->mx : 0;
}
int gfx_be_mouse_y(void *handle) {
	GfxX11 *g = handle;
	return g ? g->my : 0;
}
int gfx_be_mouse_down(void *handle) {
	GfxX11 *g = handle;
	return g ? g->mdown : 0;
}

/* Horizontal scroll delta accumulated since the last read (wheel), then cleared — drain-and-clear like the key queue.
 */
int gfx_be_scroll(void *handle) {
	GfxX11 *g = handle;
	if (!g)
		return 0;
	int s = g->scroll;
	g->scroll = 0;
	return s;
}

/* No foreign text widget on this backend: gfx always owns the keyboard, so focus is entirely the driver's
 * business (it hit-tests its own editor rect). See gfx.arche's `text_focus` / `release_text`. */
int gfx_be_text_focus(void *handle) {
	(void)handle;
	return 0;
}

int gfx_be_release_text(void *handle) {
	(void)handle;
	return 0;
}

/* No touch input on this backend — a real mouse is a FINE pointer, so on-screen touch controls are never
 * wanted here. See gfx.arche's `coarse_pointer`. */
int gfx_be_coarse_pointer(void *handle) {
	(void)handle;
	return 0;
}

/* No DOM to sandwich: a single framebuffer and plain draw order already give the right depth, so the layer
 * break is a no-op. See gfx.arche's `split`. */
void gfx_be_split(void *handle, int *px, int w, int h) {
	(void)handle;
	(void)px;
	(void)w;
	(void)h;
}

/* No stacked surfaces to order: stacking is a no-op here. See gfx.arche's `layers`. */
int gfx_be_layers(void *handle, int bgz, int fgz) {
	(void)handle;
	(void)bgz;
	(void)fgz;
	return 0;
}

/* Expose the scene window's X11 identity to sibling native backends (see gfx_x11.h). */
Display *gfx_x11_display(void *handle) {
	GfxX11 *g = handle;
	return g ? g->dpy : NULL;
}
Window gfx_x11_window(void *handle) {
	GfxX11 *g = handle;
	return g ? g->win : 0;
}

void gfx_be_close(void *handle) {
	GfxX11 *g = handle;
	if (!g)
		return;
	if (g->img)
		XDestroyImage(g->img); /* frees g->buf */
	XFreeGC(g->dpy, g->gc);
	XDestroyWindow(g->dpy, g->win);
	XCloseDisplay(g->dpy);
	free(g);
}
