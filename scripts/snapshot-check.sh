#!/bin/sh
# Whether this tree's build still resumes the snapshots (hibernated
# programs, save points) the baseline's build saved: the last recorded
# manifest, scripts/snapshot-baseline.txt, which main keeps current.
# Prints nothing, and exits 0, when they all resume.  Otherwise lists
# what stops them (a saved static or tagged type whose type changed in a
# way hibernate.c cannot carry a value across, a guest-side layout
# record changed) and exits 1: such a change must be flagged ("Breaks
# snapshots: ..." in its commit message), and the baseline recorded
# again with it.
#
# usage: sh scripts/snapshot-check.sh [--record] [baseline]
#   --record writes this build's baseline (after a flagged break, or to
#   take in a compatible change: new statics, carried type changes).
#   BLUEBOX=binary checks that build instead of building this tree's.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
record=0
if [ "${1:-}" = "--record" ]; then
	record=1
	shift
fi
baseline=${1:-$root/scripts/snapshot-baseline.txt}

if [ -z "${BLUEBOX:-}" ]; then
	(cd "$root" && swift build -c release --product bluebox >/dev/null 2>&1) || {
		echo "snapshot-check: the release build failed (swift build -c release --product bluebox)" >&2
		exit 2
	}
	BLUEBOX="$(cd "$root" && swift build -c release --show-bin-path)/bluebox"
fi
home="$root/.build/snapshot-check-home"
mkdir -p "$home"

if [ "$record" = 1 ]; then
	HOME="$home" "$BLUEBOX" fingerprint --baseline >"$baseline.new"
	mv "$baseline.new" "$baseline"
	exit 0
fi

if [ ! -f "$baseline" ]; then
	echo "snapshot-check: no baseline at $baseline (sh scripts/snapshot-check.sh --record makes one)" >&2
	exit 2
fi
if out=$(HOME="$home" "$BLUEBOX" fingerprint --check "$baseline" 2>&1); then
	exit 0
fi
echo "Breaks snapshots saved by the baseline's build ($baseline):"
printf '%s\n' "$out" | sed 's/^/  /'
echo "Flag it (\"Breaks snapshots: ...\" in the commit message and the report), or make the"
echo "change compatible (see Sources/Toolbox/hibernate.c, \"Keeping snapshots resumable\")."
echo "Then record the new baseline: sh scripts/snapshot-check.sh --record"
exit 1
