/* Fast, dependency-free CSV numeric parsing for the C baseline — a real upper bound, not libc
 * strtod/strtol (which are locale-aware, general, and ~3-5x slower; they dominated the naive baseline).
 * Single forward pass; each parser stops at the first non-digit and advances the cursor past the field.
 * Assumes well-formed input (the generated benchmark CSV). */
#ifndef ARCHE_BENCH_FAST_PARSE_H
#define ARCHE_BENCH_FAST_PARSE_H

/* Parse a signed integer at *pp; advance *pp to the first non-digit. */
static inline long fast_atol(const char **pp) {
	const char *s = *pp;
	int neg = (*s == '-');
	s += neg;
	long x = 0;
	while ((unsigned)(*s - '0') <= 9u) {
		x = x * 10 + (*s - '0');
		s++;
	}
	*pp = s;
	return neg ? -x : x;
}

/* Parse a simple decimal float (no exponent) at *pp; advance *pp to the first non-digit. */
static inline double fast_atof(const char **pp) {
	static const double P10[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8};
	const char *s = *pp;
	int neg = (*s == '-');
	s += neg;
	long mant = 0;
	while ((unsigned)(*s - '0') <= 9u) {
		mant = mant * 10 + (*s - '0');
		s++;
	}
	double val = (double)mant;
	if (*s == '.') {
		s++;
		long frac = 0;
		int k = 0;
		while ((unsigned)(*s - '0') <= 9u) {
			frac = frac * 10 + (*s - '0');
			k++;
			s++;
		}
		val += (double)frac / P10[k < 8 ? k : 8];
	}
	*pp = s;
	return neg ? -val : val;
}

/* Pointer-taking convenience wrappers (drop-in for strtod/strtol where the field start is already known). */
static inline double fast_atof_p(const char *s) {
	return fast_atof(&s);
}
static inline long fast_atol_p(const char *s) {
	return fast_atol(&s);
}

#endif
