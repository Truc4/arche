/* Wayland backend shim for the arche `gfx` device. Auto-discovered + compiled because it sits in the
 * gfx/wayland/ variant folder (alongside the wayland-scanner-generated xdg-shell-protocol.c, also
 * auto-compiled); libwayland-client is supplied by wayland/backend.arche's `#link { "wayland-client" }`.
 *
 * Same shim interface as the X11 backend (gfx_be_open/frame/w/h/present/poll/close): a resizable
 * software framebuffer. Pixels live in a wl_shm shared-memory buffer (memfd) the compositor reads;
 * the surface is an xdg-shell toplevel. The framebuffer is reallocated when the toplevel is resized,
 * so it fills the window at any size. `window` (opaque on the arche side) is this GfxWL* pointer. */
#define _GNU_SOURCE
#include "xdg-shell-client-protocol.h"
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

typedef struct {
	struct wl_display *dpy;
	struct wl_registry *reg;
	struct wl_compositor *comp;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;
	struct wl_surface *surface;
	struct xdg_surface *xsurf;
	struct xdg_toplevel *toplevel;
	struct wl_buffer *buffer;
	int *buf; /* mmap'd XRGB8888 pixels */
	size_t buf_size;
	int fd;
	int w, h;
	int pending_w, pending_h; /* latest size from a toplevel configure */
	int open;
} GfxWL;

/* ---- registry: bind the globals we need ---- */
static void reg_global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
	(void)ver;
	GfxWL *g = data;
	if (strcmp(iface, "wl_compositor") == 0)
		g->comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (strcmp(iface, "wl_shm") == 0)
		g->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (strcmp(iface, "xdg_wm_base") == 0)
		g->wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
}
static void reg_remove(void *data, struct wl_registry *r, uint32_t name) {
	(void)data;
	(void)r;
	(void)name;
}
static const struct wl_registry_listener reg_listener = {reg_global, reg_remove};

/* ---- xdg_wm_base ping/pong (keeps the connection live) ---- */
static void wm_ping(void *data, struct xdg_wm_base *b, uint32_t serial) {
	(void)data;
	xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_listener = {wm_ping};

/* ---- xdg_surface configure: must ack ---- */
static void xsurf_configure(void *data, struct xdg_surface *s, uint32_t serial) {
	(void)data;
	xdg_surface_ack_configure(s, serial);
}
static const struct xdg_surface_listener xsurf_listener = {xsurf_configure};

/* ---- xdg_toplevel: size + close (+ forward-compat stubs) ---- */
static void top_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states) {
	(void)t;
	(void)states;
	GfxWL *g = data;
	if (w > 0 && h > 0) {
		g->pending_w = w;
		g->pending_h = h;
	}
}
static void top_close(void *data, struct xdg_toplevel *t) {
	(void)t;
	((GfxWL *)data)->open = 0;
}
static void top_configure_bounds(void *data, struct xdg_toplevel *t, int32_t w, int32_t h) {
	(void)data;
	(void)t;
	(void)w;
	(void)h;
}
static void top_wm_capabilities(void *data, struct xdg_toplevel *t, struct wl_array *caps) {
	(void)data;
	(void)t;
	(void)caps;
}
static const struct xdg_toplevel_listener top_listener = {top_configure, top_close, top_configure_bounds,
                                                          top_wm_capabilities};

/* (Re)allocate the shm framebuffer + wl_buffer to w x h. */
static void create_buffer(GfxWL *g, int w, int h) {
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	if (g->buffer) {
		wl_buffer_destroy(g->buffer);
		g->buffer = NULL;
	}
	if (g->buf) {
		munmap(g->buf, g->buf_size);
		g->buf = NULL;
	}
	if (g->fd >= 0) {
		close(g->fd);
		g->fd = -1;
	}
	int stride = w * 4;
	size_t size = (size_t)stride * (size_t)h;
	int fd = memfd_create("arche-gfx", MFD_CLOEXEC);
	if (fd < 0)
		return;
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return;
	}
	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(g->shm, fd, (int32_t)size);
	g->buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	g->buf = data;
	g->buf_size = size;
	g->fd = fd;
	g->w = w;
	g->h = h;
}

void *gfx_be_open(int w, int h, char *title) {
	GfxWL *g = calloc(1, sizeof(GfxWL));
	if (!g)
		return NULL;
	g->fd = -1;
	g->open = 1;
	g->dpy = wl_display_connect(NULL);
	if (!g->dpy) {
		free(g);
		return NULL;
	}
	g->reg = wl_display_get_registry(g->dpy);
	wl_registry_add_listener(g->reg, &reg_listener, g);
	wl_display_roundtrip(g->dpy); /* receive the globals */
	if (!g->comp || !g->shm || !g->wm_base) {
		wl_display_disconnect(g->dpy);
		free(g);
		return NULL;
	}
	xdg_wm_base_add_listener(g->wm_base, &wm_listener, g);
	g->surface = wl_compositor_create_surface(g->comp);
	g->xsurf = xdg_wm_base_get_xdg_surface(g->wm_base, g->surface);
	xdg_surface_add_listener(g->xsurf, &xsurf_listener, g);
	g->toplevel = xdg_surface_get_toplevel(g->xsurf);
	xdg_toplevel_add_listener(g->toplevel, &top_listener, g);
	if (title)
		xdg_toplevel_set_title(g->toplevel, title);
	g->pending_w = w;
	g->pending_h = h;
	wl_surface_commit(g->surface);
	wl_display_roundtrip(g->dpy); /* receive the initial configure */
	create_buffer(g, w, h);
	return g;
}

int *gfx_be_frame(void *handle) {
	GfxWL *g = handle;
	if (!g)
		return NULL;
	if (g->pending_w > 0 && (g->pending_w != g->w || g->pending_h != g->h))
		create_buffer(g, g->pending_w, g->pending_h);
	return g->buf;
}
int gfx_be_w(void *handle) {
	GfxWL *g = handle;
	return g ? g->w : 0;
}
int gfx_be_h(void *handle) {
	GfxWL *g = handle;
	return g ? g->h : 0;
}

void gfx_be_present(void *handle) {
	GfxWL *g = handle;
	if (!g || !g->buffer)
		return;
	wl_surface_attach(g->surface, g->buffer, 0, 0);
	wl_surface_damage_buffer(g->surface, 0, 0, g->w, g->h);
	wl_surface_commit(g->surface);
	wl_display_flush(g->dpy);
}

int gfx_be_poll(void *handle) {
	GfxWL *g = handle;
	if (!g)
		return 0;
	wl_display_flush(g->dpy);
	struct pollfd pfd = {wl_display_get_fd(g->dpy), POLLIN, 0};
	if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN))
		wl_display_dispatch(g->dpy); /* socket readable: read + dispatch (won't block) */
	else
		wl_display_dispatch_pending(g->dpy);
	return g->open;
}

void gfx_be_close(void *handle) {
	GfxWL *g = handle;
	if (!g)
		return;
	if (g->buffer)
		wl_buffer_destroy(g->buffer);
	if (g->buf)
		munmap(g->buf, g->buf_size);
	if (g->fd >= 0)
		close(g->fd);
	if (g->toplevel)
		xdg_toplevel_destroy(g->toplevel);
	if (g->xsurf)
		xdg_surface_destroy(g->xsurf);
	if (g->surface)
		wl_surface_destroy(g->surface);
	if (g->dpy)
		wl_display_disconnect(g->dpy);
	free(g);
}
