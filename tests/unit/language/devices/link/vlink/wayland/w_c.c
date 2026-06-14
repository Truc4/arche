/* wayland-variant shim: returns 22 (via pow so the build needs libm from this variant's #link). */
#include <math.h>

int vlink_wval(void) {
	return (int)pow(22.0, 1.0);
}
