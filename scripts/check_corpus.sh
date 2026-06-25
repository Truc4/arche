#!/usr/bin/env bash
# check_corpus.sh — compile every stdlib and extras module IN CONTEXT with `-Werror`,
# failing on any warning or error.
#
# Why this exists: the regular `make test` (lit + doctests) only catches *errors* in
# library code that some test or doctest actually compiles, and never gates arche-level
# *warnings* (they are non-fatal, and `extras/gfx` is excluded from lit entirely). This
# target closes both gaps: it imports each module the way a real program does, promotes
# every lint to an error, and so a stray warning or a module that no test exercises can't
# rot unnoticed.
#
# A module is a device folder (a dir with a `*.ds.arche` datasheet). We synthesize a tiny
# driver that imports it, `arche fill` satisfies any datasheet storage requirement, then
# `arche check -Werror` analyzes it. stdlib is on the default path; extras is not (point
# ARCHE_PATH at it) and its devices select a variant/target.
set -uo pipefail

ARCHE="${ARCHE:-build/arche}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fail=0

# gate <label> <module-name> [env VAR=VAL ...]   — env vars precede the arche invocation.
gate() {
	label="$1"
	mod="$2"
	shift 2
	drv="$tmp/drv_${label//\//_}.arche"
	printf '#import { %s }\n_corpus_check :: system {}\n#run _corpus_check\n' "$mod" >"$drv"
	env "$@" "$ARCHE" fill "$drv" >/dev/null 2>&1 # satisfy datasheet storage reqs (no-op if none)
	if out="$(env "$@" "$ARCHE" check -Werror "$drv" 2>&1)"; then
		echo "ok   $label"
	else
		echo "FAIL $label"
		echo "$out" | sed 's/^/    /'
		fail=1
	fi
}

# stdlib: every dir with a datasheet is an importable device, resolved by name on the default path.
for ds in stdlib/*/*.ds.arche; do
	mod="$(basename "$(dirname "$ds")")"
	gate "stdlib/$mod" "$mod"
done

# extras: off the default path (ARCHE_PATH), and variant/target devices.
gate "extras/platform" platform ARCHE_PATH=extras
for vd in extras/gfx/*/backend.arche; do
	[ -e "$vd" ] || continue
	v="$(basename "$(dirname "$vd")")"
	gate "extras/gfx[$v]" gfx ARCHE_PATH=extras "ARCHE_SELECT=gfx=$v"
done

if [ "$fail" -ne 0 ]; then
	echo "corpus check FAILED — fix the warning/error above, or @allow(<slug>) if intentional"
	exit 1
fi
echo "corpus check passed"
