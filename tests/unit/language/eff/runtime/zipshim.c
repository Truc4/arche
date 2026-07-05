/* Shim for eff_zip.arche / eff_zip_fmap.arche: independent single-return externs the `zip` product
 * combines — a buffer pointer + two scalars (the gfx px/w/h shape), plus two scalars for the `|> combine`
 * fold. */
int geta(int x) {
	return x + 1;
}
int getb(int x) {
	return x + 2;
}
static int ZBUF[4] = {11, 22, 33, 0};
int *zbuf(int h) {
	(void)h;
	return ZBUF;
}
int zw(int h) {
	(void)h;
	return 640;
}
int zh(int h) {
	(void)h;
	return 480;
}
