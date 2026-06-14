/* Headless backend shim for the arche `gfx` device. Auto-discovered + compiled because it sits in the
 * gfx/headless/ variant folder; no system library (pure libc), so no `#link`.
 *
 * Same software-framebuffer model as the X11/Wayland shims (the SHIM owns the pixel buffer; arche asks
 * for the current frame, fills it, calls present), but there is NO window: present optionally writes the
 * buffer to a PPM, and poll runs a fixed frame budget so a `while (poll)` loop self-terminates with no
 * display. This is what lets `gfx.clear/rect/circle` be pixel-tested in CI. `window` (opaque on the arche
 * side) is the GfxHL* pointer; pixels are 0xRRGGBB ints.
 *
 * Environment:
 *   GFX_HEADLESS_DUMP    — if set, present() writes the current framebuffer as a binary PPM (P6) here.
 *   GFX_HEADLESS_FRAMES  — number of frames a `while (poll)` loop runs before poll returns 0 (default 1). */
#include <stdio.h>
#include <stdlib.h>

typedef struct {
	int *buf; /* w*h pixels, 0xRRGGBB, row-major (calloc'd) */
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
	g->buf = calloc((size_t)w * (size_t)h, 4);
	if (!g->buf) {
		free(g);
		return NULL;
	}
	const char *fb = getenv("GFX_HEADLESS_FRAMES");
	g->frames_left = (fb && *fb) ? atoi(fb) : 1;
	if (g->frames_left < 1)
		g->frames_left = 1;
	return g;
}

int *gfx_be_frame(void *handle) {
	GfxHL *g = handle;
	return g ? g->buf : NULL;
}
int gfx_be_w(void *handle) {
	GfxHL *g = handle;
	return g ? g->w : 0;
}
int gfx_be_h(void *handle) {
	GfxHL *g = handle;
	return g ? g->h : 0;
}

void gfx_be_present(void *handle) {
	GfxHL *g = handle;
	if (!g || !g->buf)
		return;
	const char *path = getenv("GFX_HEADLESS_DUMP");
	if (!path || !*path)
		return;
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", g->w, g->h);
	int n = g->w * g->h;
	for (int i = 0; i < n; i++) {
		unsigned int px = (unsigned int)g->buf[i];
		unsigned char rgb[3] = {(unsigned char)((px >> 16) & 0xFF), (unsigned char)((px >> 8) & 0xFF),
		                        (unsigned char)(px & 0xFF)};
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

int gfx_be_poll(void *handle) {
	GfxHL *g = handle;
	if (!g)
		return 0;
	if (g->frames_left > 0)
		g->frames_left--;
	return g->frames_left > 0; /* 0 once the budget is spent → a `while (poll)` loop exits */
}

void gfx_be_close(void *handle) {
	GfxHL *g = handle;
	if (!g)
		return;
	free(g->buf);
	free(g);
}
