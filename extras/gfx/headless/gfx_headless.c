/* Headless backend shim for the arche `gfx` device. Auto-discovered + compiled because it sits in the
 * gfx/headless/ variant folder; no system library (pure libc), so no `#link`.
 *
 * Same software-framebuffer model as the X11/Wayland shims, but there is NO window: the DRIVER owns the
 * pixel buffer (the arche `Framebuffer` pool), and `present` receives it (`px`) and optionally writes it to
 * a PPM. poll runs a fixed frame budget so a `while (poll)` loop self-terminates with no display. This is
 * what lets `gfx.clear/rect/circle` be pixel-tested in CI. `window` (opaque on the arche side) is the GfxHL*
 * pointer; pixels are 0xRRGGBB ints.
 *
 * Environment:
 *   GFX_HEADLESS_DUMP    — if set, present() writes the current framebuffer as a binary PPM (P6) here.
 *   GFX_HEADLESS_FRAMES  — number of frames a `while (poll)` loop runs before poll returns 0 (default 1). */
#include <stdio.h>
#include <stdlib.h>

typedef struct {
	int w, h;
	int frames_left; /* poll budget; reaches 0 → loop exits */
} GfxHL;

void *gfx_be_open(int w, int h, char *title) {
	(void)title;
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	GfxHL *g = calloc(1, sizeof(GfxHL));
	if (!g)
		return NULL;
	g->w = w;
	g->h = h;
	const char *fb = getenv("GFX_HEADLESS_FRAMES");
	g->frames_left = (fb && *fb) ? atoi(fb) : 1;
	if (g->frames_left < 1)
		g->frames_left = 1;
	return g;
}

int gfx_be_w(void *handle) {
	GfxHL *g = handle;
	return g ? g->w : 0;
}
int gfx_be_h(void *handle) {
	GfxHL *g = handle;
	return g ? g->h : 0;
}

/* Present the CALLER's framebuffer (the driver-owned `Framebuffer` pool, passed as `px` + length). Optionally
 * dumps it as a binary PPM. No shim-owned buffer — pixels live in the arche pool. */
void gfx_be_present(void *handle, int *px, int w, int h) {
	(void)handle;
	if (!px)
		return;
	const char *path = getenv("GFX_HEADLESS_DUMP");
	if (!path || !*path)
		return;
	/* Write to a sibling temp then rename over `path` — present can fire every frame, so an ATOMIC publish
	 * lets a watcher/test read a whole PPM instead of a half-written one (torn read). */
	char tmp[2048];
	int tl = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	if (tl < 0 || tl >= (int)sizeof(tmp))
		return;
	FILE *f = fopen(tmp, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	int n = w * h;
	for (int i = 0; i < n; i++) {
		unsigned int p = (unsigned int)px[i];
		unsigned char rgb[3] = {(unsigned char)((p >> 16) & 0xFF), (unsigned char)((p >> 8) & 0xFF),
		                        (unsigned char)(p & 0xFF)};
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
	rename(tmp, path);
}

int gfx_be_poll(void *handle) {
	GfxHL *g = handle;
	if (!g)
		return 0;
	if (g->frames_left > 0)
		g->frames_left--;
	return g->frames_left > 0; /* 0 once the budget is spent → a `while (poll)` loop exits */
}

/* Headless has no input device — the horizontal axis is always 0. */
int gfx_be_axis_x(void *handle) {
	(void)handle;
	return 0;
}

void gfx_be_close(void *handle) {
	GfxHL *g = handle;
	if (!g)
		return;
	free(g);
}

/* No discrete key input wired for this backend. */
int gfx_be_key(void *handle) {
	(void)handle;
	return 0;
}
int gfx_be_mouse_x(void *handle) {
	(void)handle;
	return 0;
}
int gfx_be_mouse_y(void *handle) {
	(void)handle;
	return 0;
}
int gfx_be_mouse_down(void *handle) {
	(void)handle;
	return 0;
}
int gfx_be_scroll(void *handle) {
	(void)handle;
	return 0;
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
