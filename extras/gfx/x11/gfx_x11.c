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
#include <stdlib.h>
#include <string.h>

typedef struct {
	Display *dpy;
	Window win;
	GC gc;
	XImage *img;
	int *buf; /* XImage-owned pixel store (freed by XDestroyImage) */
	int w, h;
	Atom wm_delete;
	int open;
} GfxX11;

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
	XSelectInput(dpy, win, ExposureMask | KeyPressMask | StructureNotifyMask);
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

/* The current writable framebuffer (arche slices it to w*h). */
int *gfx_be_frame(void *handle) {
	GfxX11 *g = handle;
	return g ? g->buf : NULL;
}
int gfx_be_w(void *handle) {
	GfxX11 *g = handle;
	return g ? g->w : 0;
}
int gfx_be_h(void *handle) {
	GfxX11 *g = handle;
	return g ? g->h : 0;
}

void gfx_be_present(void *handle) {
	GfxX11 *g = handle;
	if (!g || !g->img)
		return;
	XPutImage(g->dpy, g->win, g->gc, g->img, 0, 0, 0, 0, (unsigned)g->w, (unsigned)g->h);
	XFlush(g->dpy);
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
		} else if (ev.type == KeyPress) {
			g->open = 0;
		}
	}
	return g->open;
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
