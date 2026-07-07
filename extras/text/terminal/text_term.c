/* Terminal (dev) backend shim for the arche `text` device. Auto-discovered + compiled because it sits in the
 * text/terminal/ variant folder; pure libc, so no `#link`. Under the native `arche run` dev loop there is no
 * DOM to render into, so each text.draw is echoed to stdout. The string is a (ptr,len) pair — arche `[]char`
 * lowers to a bare pointer, so the length arrives as a separate arg (no strlen, and it need not be
 * NUL-terminated → print with `%.*s`). Deliberately plain: this is a dev aid, not a renderer. */
#include <stdio.h>

void text_be_draw(int x, int y, const char *p, int n, int size, int color) {
	printf("[text] (%d,%d) size=%d #%06x %.*s\n", x, y, size, color & 0xffffff, n, p);
}

void text_be_clear(void) {
	printf("[text] clear\n");
}
