/* C shim for the `clink` device. Auto-discovered + linked by the compiler because it lives in the
 * device folder. Calls pow(), so the build must link libm — supplied by clink.arche's `#link { "m" }`. */
#include <math.h>

/* arche `float` is f32, so the shim takes/returns C `float` (use powf, not pow). */
float clink_cbrt(float x) {
	return powf(x, 1.0f / 3.0f);
}
