/* x11-variant shim: returns 11 (via pow so the build needs libm from this variant's #link). */
#include <math.h>

int vlink_xval(void) {
	return (int)pow(11.0, 1.0);
}
