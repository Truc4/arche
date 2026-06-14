/* C shim for the `clink` device. Auto-discovered + linked by the compiler because it lives in the
 * device folder. Calls pow(), so the build must link libm — supplied by clink.arche's `#link { "m" }`. */
#include <math.h>

double clink_cbrt(double x) {
	return pow(x, 1.0 / 3.0);
}
