#ifndef ARCHE_GFX_X11_H
#define ARCHE_GFX_X11_H
/* Accessors that expose the gfx scene window's X11 identity to SIBLING native backends — e.g. an
 * embedded editor that creates a child window parented to the scene window (the native analog of the
 * browser DOM backend overlaying a <textarea> on the <canvas>). The opaque arche `window` handle is a
 * `GfxX11*`; these hand back its Display + Window without exposing the struct layout. Implemented in
 * gfx_x11.c; another backend's shim includes this header and links against the same binary. */
#include <X11/Xlib.h>

Display *gfx_x11_display(void *handle);
Window gfx_x11_window(void *handle);

#endif
