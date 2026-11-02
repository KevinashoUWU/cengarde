#!/bin/sh
# Compares the engine's decisions at two revisions, exactly (bench/dectrace.c):
# each revision's pure headers run the same scenarios with the same seeds
# under a virtual clock, and every piece's trace must hash the same. For a
# cleanup or a refactor that should change nothing: the lab cannot tell
# (an A/A of the same build differs 2 % in a typical case and 20-36 % in
# dominant or protect, bench/README.md), this tells exactly.
#
#   bench/dectrace.sh REV_A REV_B [SEEDS...]
#
# Each revision is checked out in a temporary worktree; it compiles its own
# bench/dectrace.c when it has one, else this checkout's (an older engine's
# API may not fit it: then that revision does not build, and it says so).
# Seeds: 2026 1 2 by default. Exit 0 when every trace matches, 1 when one
# does not (the first difference of each, with -v traces, under $OUT), 2 on
# an error. OUT: where the traces go (default a temporary directory, kept).
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

[ $# -ge 2 ] || { echo "usage: bench/dectrace.sh REV_A REV_B [SEEDS...]" >&2; exit 2; }
A=$1
B=$2
shift 2
SEEDS=${*:-2026 1 2}
SCENARIOS="equal het fade lossy death dominant idle"
SRC=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$(mktemp -d "${TMPDIR:-/tmp}/dectrace.XXXXXX")}
CC=${CC:-cc}
mkdir -p "$OUT"

build() { # REV TAG: $OUT/TAG/dectrace from REV's headers
	wt=$OUT/$2/wt
	rm -rf "$wt"
	git -C "$SRC" worktree add -q --detach "$wt" "$1" || { echo "dectrace: no revision $1" >&2; exit 2; }
	c=$wt/bench/dectrace.c
	[ -f "$c" ] || c=$SRC/bench/dectrace.c
	if ! $CC -std=c11 -D_GNU_SOURCE -O2 -I"$wt/engine/src" -o "$OUT/$2/dectrace" "$c" 2>"$OUT/$2/build.log"; then
		echo "dectrace: $1 does not build with $c (an older API?): $OUT/$2/build.log" >&2
		git -C "$SRC" worktree remove --force "$wt"
		exit 2
	fi
	git -C "$SRC" worktree remove --force "$wt"
}

mkdir -p "$OUT/a" "$OUT/b"
build "$A" a
build "$B" b
diffs=0
for s in $SCENARIOS; do
	for seed in $SEEDS; do
		"$OUT/a/dectrace" "$s" "$seed" >"$OUT/a/$s-$seed.txt"
		"$OUT/b/dectrace" "$s" "$seed" >"$OUT/b/$s-$seed.txt"
		if cmp -s "$OUT/a/$s-$seed.txt" "$OUT/b/$s-$seed.txt"; then
			printf '%-9s %-5s same\n' "$s" "$seed"
		else
			diffs=$((diffs + 1))
			# which pieces, and the first tick where the full traces part
			pieces=$(diff "$OUT/a/$s-$seed.txt" "$OUT/b/$s-$seed.txt" | sed -n 's/^> \([a-z0-9]*\) .*/\1/p' | grep -v scenario | tr '\n' ' ')
			"$OUT/a/dectrace" "$s" "$seed" -v >"$OUT/a/$s-$seed-v.txt"
			"$OUT/b/dectrace" "$s" "$seed" -v >"$OUT/b/$s-$seed-v.txt"
			first=$(diff "$OUT/a/$s-$seed-v.txt" "$OUT/b/$s-$seed-v.txt" | sed -n 's/^< \([0-9]*\) \([a-z0-9]*\).*/\2 at \1 ms/p' | head -1)
			printf '%-9s %-5s DIFFERENT: %s(first: %s)\n' "$s" "$seed" "$pieces" "$first"
		fi
	done
done
echo "dectrace: $A against $B: $diffs of $(($(echo "$SCENARIOS" | wc -w) * $(echo "$SEEDS" | wc -w))) traces differ; traces in $OUT"
[ "$diffs" = 0 ]
