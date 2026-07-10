/* textedit=x11in — "OS input, native surface" editor backend.
 *
 * Creates an X11 CHILD window parented to the gfx scene window (via gfx_x11_window()), so the editor is
 * embedded in the one scene window — NOT a separate top-level — the native analog of the browser DOM
 * backend's <textarea> over the <canvas>. The OS delivers real focus + key/repeat events to the child;
 * this shim maintains the edit buffer and draws the text with a CORE X SERVER FONT (XDrawString — plain
 * Xlib, no extra cflags/deps). arche only positions it (place) and pulls the buffer back (text).
 *
 * The child runs on its OWN Display connection so the gfx backend's poll loop (a different connection)
 * never steals the child's key events — each connection has its own event queue. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdlib.h>
#include <string.h>
#include "../../../gfx/x11/gfx_x11.h"

#define ED_CAP 4096
#define BG 0x0e121bUL /* editor background (matches the dom backend) */
#define FG 0xcdd6f4UL /* text colour */

typedef struct {
	Display *dpy;
	Window win;
	GC gc;
	XFontStruct *font;
	int fasc, fh; /* font ascent + line height */
	char buf[ED_CAP];
	int len, cursor;
	int w, h;        /* current child size (px) */
	int run_pending; /* Ctrl-R edge, drained by poll_run */
	int ready;
} Editor;

static Editor E;

static void ins(char c) {
	if (E.len >= ED_CAP - 1)
		return;
	memmove(E.buf + E.cursor + 1, E.buf + E.cursor, (size_t)(E.len - E.cursor));
	E.buf[E.cursor] = c;
	E.len++;
	E.cursor++;
	E.buf[E.len] = 0;
}

static void handle_key(XKeyEvent *ke) {
	char s[16];
	KeySym ks;
	int n = XLookupString(ke, s, sizeof(s), &ks, NULL);
	if ((ks == XK_r || ks == XK_R) && (ke->state & ControlMask)) {
		E.run_pending = 1;
		return;
	}
	if (ks == XK_Left) {
		if (E.cursor > 0)
			E.cursor--;
		return;
	}
	if (ks == XK_Right) {
		if (E.cursor < E.len)
			E.cursor++;
		return;
	}
	if (ks == XK_BackSpace) {
		if (E.cursor > 0) {
			memmove(E.buf + E.cursor - 1, E.buf + E.cursor, (size_t)(E.len - E.cursor));
			E.len--;
			E.cursor--;
			E.buf[E.len] = 0;
		}
		return;
	}
	if (ks == XK_Return || ks == XK_KP_Enter) {
		ins('\n');
		return;
	}
	if (ks == XK_Tab) {
		ins(' ');
		ins(' ');
		return;
	}
	for (int i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c >= 32 && c < 127)
			ins((char)c);
	}
}

static void redraw(void) {
	if (!E.ready)
		return;
	XSetForeground(E.dpy, E.gc, BG);
	XFillRectangle(E.dpy, E.win, E.gc, 0, 0, (unsigned)E.w, (unsigned)E.h);
	XSetForeground(E.dpy, E.gc, FG);
	const int x0 = 6;
	int y = 6 + E.fasc;
	int ls = 0; /* line start */
	int cur_x = x0, cur_y = 6; /* cursor pixel pos */
	for (int i = 0; i <= E.len; i++) {
		if (i == E.len || E.buf[i] == '\n') {
			int llen = i - ls;
			if (E.font)
				XDrawString(E.dpy, E.win, E.gc, x0, y, E.buf + ls, llen);
			if (E.cursor >= ls && E.cursor <= i) {
				int col = E.cursor - ls;
				int px = E.font ? XTextWidth(E.font, E.buf + ls, col) : col * 8;
				cur_x = x0 + px;
				cur_y = y - E.fasc;
			}
			y += E.fh;
			ls = i + 1;
		}
	}
	/* cursor caret */
	XFillRectangle(E.dpy, E.win, E.gc, cur_x, cur_y, 2, (unsigned)E.fh);
	XFlush(E.dpy);
}

void textedit_be_open(void *gfxhandle, char *seed, int n) {
	if (E.ready)
		return;
	Window parent = gfx_x11_window(gfxhandle);
	if (!parent)
		return;
	E.dpy = XOpenDisplay(NULL);
	if (!E.dpy)
		return;
	int scr = DefaultScreen(E.dpy);
	E.win = XCreateSimpleWindow(E.dpy, parent, 0, 0, 1, 1, 0, BlackPixel(E.dpy, scr), BG);
	XSelectInput(E.dpy, E.win, KeyPressMask | ButtonPressMask | ExposureMask | FocusChangeMask);
	E.gc = XCreateGC(E.dpy, E.win, 0, NULL);
	E.font = XLoadQueryFont(E.dpy, "-*-*-medium-r-normal--14-*-*-*-*-*-*-*");
	if (!E.font)
		E.font = XLoadQueryFont(E.dpy, "fixed");
	if (E.font) {
		XSetFont(E.dpy, E.gc, E.font->fid);
		E.fasc = E.font->ascent;
		E.fh = E.font->ascent + E.font->descent + 2;
	} else {
		E.fasc = 12;
		E.fh = 18;
	}
	XMapWindow(E.dpy, E.win);
	int k = 0;
	for (; k < n && k < ED_CAP - 1 && seed[k]; k++)
		E.buf[k] = seed[k];
	E.buf[k] = 0;
	E.len = k;
	E.cursor = k;
	E.ready = 1;
	XFlush(E.dpy);
}

void textedit_be_place(int x, int y, int w, int h) {
	if (!E.ready)
		return;
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	XMoveResizeWindow(E.dpy, E.win, x, y, (unsigned)w, (unsigned)h);
	E.w = w;
	E.h = h;
	XFlush(E.dpy);
}

/* Called every frame: drain the child window's OS input, redraw, and export the buffer to arche's source. */
void textedit_be_text(char *buf, int cap) {
	if (!E.ready) {
		if (cap > 0)
			buf[0] = 0;
		return;
	}
	while (XPending(E.dpy)) {
		XEvent ev;
		XNextEvent(E.dpy, &ev);
		if (ev.type == KeyPress)
			handle_key(&ev.xkey);
		else if (ev.type == ButtonPress)
			XSetInputFocus(E.dpy, E.win, RevertToParent, CurrentTime);
	}
	redraw();
	int k = E.len;
	if (k > cap - 1)
		k = cap - 1;
	memcpy(buf, E.buf, (size_t)k);
	buf[k] = 0;
}

int textedit_be_poll_run(void) {
	int r = E.run_pending;
	E.run_pending = 0;
	return r;
}
