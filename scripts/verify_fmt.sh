#!/usr/bin/env bash
# verify-fmt: `arche fmt` must never CORRUPT a file and must be IDEMPOTENT, across the whole corpus.
#
# This is the net that was missing: the formatter was silently turning some files into NUL/heap-pointer
# garbage (data loss) and nothing tested it — `make test` stayed green over a real, file-destroying bug.
# A NUL byte in formatter output is never valid arche; and `fmt(fmt(x))` must equal `fmt(x)`.
#
# Files that don't parse (intentionally-malformed error-test fixtures) format to a parse error (nonzero
# exit) and are skipped — same policy as verify-syntax.
set -u
ARCHE="${ARCHE:-build/arche}"
tmp1="$(mktemp --suffix=.arche)"
tmp2="$(mktemp --suffix=.arche)"
trap 'rm -f "$tmp1" "$tmp2"' EXIT

fail=0 checked=0 skipped=0
for f in "$@"; do
	if ! "$ARCHE" fmt "$f" >"$tmp1" 2>/dev/null; then
		skipped=$((skipped + 1)) # fmt couldn't parse/format it (error-test fixture) — not our concern
		continue
	fi
	checked=$((checked + 1))
	# Corruption: any NUL byte in the output is never valid (catches the heap-garbage data-loss bug).
	if [ "$(wc -c <"$tmp1")" -ne "$(tr -d '\0' <"$tmp1" | wc -c)" ]; then
		echo "verify-fmt: CORRUPT (NUL bytes in fmt output): $f"
		fail=1
		continue
	fi
	# Idempotency: formatting the formatted output must reproduce it exactly.
	if ! "$ARCHE" fmt "$tmp1" >"$tmp2" 2>/dev/null || ! cmp -s "$tmp1" "$tmp2"; then
		echo "verify-fmt: NON-IDEMPOTENT (fmt(fmt(x)) != fmt(x)): $f"
		fail=1
		continue
	fi
done

if [ "$fail" -eq 0 ]; then
	echo "verify-fmt: all $checked formatted files are corruption-free and idempotent ($skipped skipped)"
fi
exit $fail
