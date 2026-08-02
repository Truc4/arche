/* textview=x11gtk — the output pane as a read-only GtkTextView embedded in the gfx scene window.
 *
 * A non-editable GtkTextView (word-wrap, monospace) inside a GtkScrolledWindow (automatic scrollbars),
 * reparented into the gfx scene window via gfx_x11_window() — so long compiler output WRAPS and SCROLLS
 * natively instead of clipping like the framebuffer backend. arche pushes the text + rect each frame. GTK
 * runs its own GDK/X connection, pumped once per frame. Requires gfx=x11. Mirrors textedit/x11gtk. */
#include "../../../gfx/x11/gfx_x11.h"
#include <X11/Xlib.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

static struct {
	GtkWidget *win;  /* the reparented GtkWindow */
	GtkWidget *view; /* read-only GtkTextView */
	GtkTextBuffer *tb;
	Display *xdpy;
	Window xid;
	int ready;
} V;

/* Theme from the DRIVER's palette (bg/fg, 0xRRGGBB) — no colours hardcoded here. Screen-wide at APPLICATION
 * priority; only our named node is affected. */
static void apply_css(int bg, int fg) {
	char css[256];
	snprintf(css, sizeof css,
	         "window { background-color:#%06x; }"
	         "#arche-output, #arche-output text { background-color:#%06x; color:#%06x; }",
	         bg & 0xffffff, bg & 0xffffff, fg & 0xffffff);
	GtkCssProvider *p = gtk_css_provider_new();
	gtk_css_provider_load_from_data(p, css, -1, NULL);
	gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(p),
	                                          GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(p);
}

static void ensure(void *gfxhandle, int bg, int fg) {
	if (V.ready)
		return;
	Window parent = gfx_x11_window(gfxhandle);
	if (!parent)
		return;
	gdk_set_allowed_backends("x11"); /* reparent needs an X11 GdkWindow (see textedit/x11gtk) */
	if (!gtk_init_check(NULL, NULL))
		return;
	V.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_decorated(GTK_WINDOW(V.win), FALSE);
	GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	V.view = gtk_text_view_new();
	gtk_widget_set_name(V.view, "arche-output");
	apply_css(bg, fg);
	gtk_text_view_set_editable(GTK_TEXT_VIEW(V.view), FALSE);
	gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(V.view), FALSE);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(V.view), TRUE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(V.view), GTK_WRAP_WORD_CHAR);
	V.tb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(V.view));
	gtk_container_add(GTK_CONTAINER(sw), V.view);
	gtk_container_add(GTK_CONTAINER(V.win), sw);
	gtk_widget_realize(V.win);
	gtk_widget_show_all(V.win);
	GdkWindow *gw = gtk_widget_get_window(V.win);
	V.xdpy = GDK_WINDOW_XDISPLAY(gw);
	V.xid = GDK_WINDOW_XID(gw);
	XReparentWindow(V.xdpy, V.xid, parent, 0, 0);
	XMapWindow(V.xdpy, V.xid);
	XFlush(V.xdpy);
	V.ready = 1;
}

void textview_be_render(void *gfxhandle, char *buf, int n, int x, int y, int w, int h, int bg, int fg) {
	ensure(gfxhandle, bg, fg);
	if (!V.ready)
		return;
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	/* Only reset the buffer when the text actually changed (else selection/scroll would reset each frame). */
	GtkTextIter a, b;
	gtk_text_buffer_get_bounds(V.tb, &a, &b);
	char *cur = gtk_text_buffer_get_text(V.tb, &a, &b, FALSE);
	int same = cur && (int)strlen(cur) == n && memcmp(cur, buf, (size_t)n) == 0;
	if (cur)
		g_free(cur);
	if (!same)
		gtk_text_buffer_set_text(V.tb, buf, n);
	gtk_window_resize(GTK_WINDOW(V.win), w, h);
	XMoveResizeWindow(V.xdpy, V.xid, x, y, (unsigned)w, (unsigned)h);
	int guard = 0;
	while (gtk_events_pending() && guard++ < 100000)
		gtk_main_iteration_do(FALSE);
	XFlush(V.xdpy);
}
