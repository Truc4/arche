/* textedit=x11gtk — the editor as a REAL GTK GtkTextView, embedded in the gfx scene window.
 *
 * Builds a decoration-less GtkWindow holding a GtkTextView, realizes it, then XReparentWindow()s its X
 * window into the gfx scene window (gfx_x11_window()) so the toolkit widget is a CHILD of the one scene
 * window — not a separate top-level. GTK renders + edits it natively (cursor, selection, IME, clipboard);
 * this shim only seeds it, positions it, pulls the text back, and watches for Ctrl-R.
 *
 * GTK runs its own GDK display connection, so its event loop (pumped once per frame) never races the gfx
 * backend's own X connection. Requires gfx=x11. */
#include "../../../gfx/x11/gfx_x11.h"
#include <X11/Xlib.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

#define ED_CAP 4096

static struct {
	GtkWidget *win;  /* the reparented GtkWindow */
	GtkWidget *view; /* GtkTextView */
	GtkTextBuffer *tb;
	Display *xdpy; /* GDK's X display connection */
	Window xid;    /* the GtkWindow's X id (child of the scene window) */
	int run_pending;
	int ready;
} G;

/* Theme the widget from the DRIVER's palette (bg/fg, 0xRRGGBB) — no colours hardcoded here. Applied
 * screen-wide at APPLICATION priority; our named node is the only editor widget in the app. */
static void apply_css(int bg, int fg) {
	char css[256];
	snprintf(css, sizeof css,
	         "window { background-color:#%06x; }"
	         "#arche-editor, #arche-editor text {"
	         "  background-color:#%06x; color:#%06x; caret-color:#%06x; }",
	         bg & 0xffffff, bg & 0xffffff, fg & 0xffffff, fg & 0xffffff);
	GtkCssProvider *p = gtk_css_provider_new();
	gtk_css_provider_load_from_data(p, css, -1, NULL);
	gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(p),
	                                          GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(p);
}

static gboolean on_key(GtkWidget *w, GdkEventKey *e, gpointer u) {
	(void)w;
	(void)u;
	if ((e->keyval == GDK_KEY_r || e->keyval == GDK_KEY_R) && (e->state & GDK_CONTROL_MASK)) {
		G.run_pending = 1;
		return TRUE;
	}
	return FALSE;
}

void textedit_be_open(void *gfxhandle, char *seed, int n, int bg, int fg) {
	if (G.ready)
		return;
	Window parent = gfx_x11_window(gfxhandle);
	if (!parent)
		return;
	/* Force GDK onto X11 (not Wayland) so we can reparent into the gfx X11 scene window — otherwise
	 * GDK_WINDOW_XID() asserts on a non-X11 GdkWindow. Must precede gtk_init. */
	gdk_set_allowed_backends("x11");
	if (!gtk_init_check(NULL, NULL))
		return;
	G.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_decorated(GTK_WINDOW(G.win), FALSE);
	GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
	G.view = gtk_text_view_new();
	gtk_widget_set_name(G.view, "arche-editor");
	apply_css(bg, fg);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(G.view), TRUE);
	G.tb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(G.view));
	int k = 0;
	for (; k < n && k < ED_CAP - 1 && seed[k]; k++)
		;
	gtk_text_buffer_set_text(G.tb, seed, k);
	g_signal_connect(G.view, "key-press-event", G_CALLBACK(on_key), NULL);
	gtk_container_add(GTK_CONTAINER(sw), G.view);
	gtk_container_add(GTK_CONTAINER(G.win), sw);
	gtk_widget_realize(G.win);
	gtk_widget_show_all(G.win);
	GdkWindow *gw = gtk_widget_get_window(G.win);
	G.xdpy = GDK_WINDOW_XDISPLAY(gw);
	G.xid = GDK_WINDOW_XID(gw);
	XReparentWindow(G.xdpy, G.xid, parent, 0, 0); /* embed into the scene window */
	XMapWindow(G.xdpy, G.xid);
	XFlush(G.xdpy);
	G.ready = 1;
}

void textedit_be_place(int x, int y, int w, int h) {
	if (!G.ready)
		return;
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	gtk_window_resize(GTK_WINDOW(G.win), w, h);
	XMoveResizeWindow(G.xdpy, G.xid, x, y, (unsigned)w, (unsigned)h);
	XFlush(G.xdpy);
}

/* Pump the GTK loop (renders + native input), then export the buffer text to arche's source. */
void textedit_be_text(char *buf, int cap) {
	if (!G.ready) {
		if (cap > 0)
			buf[0] = 0;
		return;
	}
	int guard = 0;
	while (gtk_events_pending() && guard++ < 100000)
		gtk_main_iteration_do(FALSE);
	GtkTextIter a, b;
	gtk_text_buffer_get_bounds(G.tb, &a, &b);
	char *txt = gtk_text_buffer_get_text(G.tb, &a, &b, FALSE);
	int k = 0;
	if (txt) {
		int L = (int)strlen(txt);
		k = L > cap - 1 ? cap - 1 : L;
		memcpy(buf, txt, (size_t)k);
		g_free(txt);
	}
	buf[k] = 0;
}

int textedit_be_poll_run(void) {
	int r = G.run_pending;
	G.run_pending = 0;
	return r;
}
