/* X11 (Xlib) backend shim for the arche `gfx` device. Auto-discovered + compiled because it sits in
 * the gfx/x11/ variant folder; libX11 is supplied by x11/backend.arche's `#link { "X11" }`.
 *
 * A software framebuffer: the arche side hands us a width*height int buffer (0xRRGGBB), we copy it
 * into an XImage and XPutImage it to the window. Pixels travel inline in the X protocol — no MIT-SHM,
 * so no shared-memory fd to manage. `window` (opaque on the arche side) is this GfxX11* pointer. */
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

void *gfx_be_open(int w, int h, char *title) {
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy)
		return NULL;
	int screen = DefaultScreen(dpy);
	Window root = RootWindow(dpy, screen);
	unsigned long black = BlackPixel(dpy, screen);
	Window win = XCreateSimpleWindow(dpy, root, 0, 0, (unsigned)w, (unsigned)h, 0, black, black);
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
	g->w = w;
	g->h = h;
	g->wm_delete = wm_delete;
	g->open = 1;
	g->buf = malloc((size_t)w * (size_t)h * 4);
	g->img = XCreateImage(dpy, DefaultVisual(dpy, screen), (unsigned)DefaultDepth(dpy, screen), ZPixmap, 0,
	                      (char *)g->buf, (unsigned)w, (unsigned)h, 32, 0);
	return g;
}

void gfx_be_blit(void *handle, int *px) {
	GfxX11 *g = handle;
	if (!g || !g->img || !px)
		return;
	memcpy(g->buf, px, (size_t)g->w * (size_t)g->h * 4);
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
		if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == g->wm_delete)
			g->open = 0;
		else if (ev.type == KeyPress)
			g->open = 0;
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
