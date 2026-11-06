#!/bin/sh
# The guards on the C targets' state, for hibernating a program (a
# snapshot saves the state sections and the tracked blocks):
#  - no symbol of a C target in plain __data, __bss or __common (a file
#    the state header, Sources/CCore/state/bbstate.h, did not reach);
#  - no raw malloc, calloc, realloc, free, strdup or strndup in their
#    sources outside the allocator (bbmem.c): bb_malloc and friends, or
#    bb_host_malloc and friends for the host's own memory;
#  - the census against the pointer map and the allowlist: every saved
#    static and tagged type in Sources/Toolbox/statemap_gen.c as the
#    DWARF has it now, and none holding the host's state (a lock, a
#    library's handle, a descriptor) unless scripts/state-allow.txt names
#    it (scripts/state-map.py --check).
#
# usage: sh scripts/state-check.sh [build dir]
#   The build dir is a configuration's intermediates, by default the debug
#   build's (.build/out/Intermediates.noindex/bluebox.build/Debug, after
#   `swift build` or `swift test`).  Prints what it finds; fails if
#   anything.
set -eu
LC_ALL=C
export LC_ALL

root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-"$root/.build/out/Intermediates.noindex/bluebox.build/Debug"}
targets='CCore CPU68K CPUPPC ForkStore Toolbox'
status=0
raw=$(mktemp)
trap 'rm -f "$raw"' EXIT

# Symbols in plain data sections.
for t in $targets; do
	dir="$build/$t-t.build/Objects-normal"
	if [ ! -d "$dir" ]; then
		echo "state-check: no objects for $t in $build (build first)" >&2
		exit 2
	fi
	for o in "$dir"/*/*.o; do
		nm -m "$o" | awk -v obj="$t/${o##*/}" '
			/\((__DATA,__(data|bss|common))\)|\(common\)/ && $NF !~ /^ltmp/ {
				print "state-check: " obj ": " $NF " is in plain " $2
				found = 1
			}
			END { exit found }' || status=1
	done
done

# Raw allocation calls (comments and string literals left out).
for t in $targets; do
	find "$root/Sources/$t" -type f \( -name '*.c' -o -name '*.m' -o -name '*.h' \) ! -name bbmem.c
done | sort | while IFS= read -r f; do
	sed -E -e 's|/\*.*\*/||g' -e 's|//.*||' -e 's/"([^"\\]|\\.)*"/""/g' -e 's|^[[:space:]]*\*.*||' "$f" |
		grep -nE '(^|[^A-Za-z0-9_.>])(malloc|calloc|realloc|reallocf|free|strdup|strndup|valloc|aligned_alloc|posix_memalign)[[:space:]]*\(' |
		sed "s|^|state-check: ${f#"$root"/}:|; s|\$| (use bb_* or bb_host_*)|" || true
done > "$raw"
if [ -s "$raw" ]; then
	cat "$raw"
	status=1
fi

# The census against the map and the allowlist.
python3 -I "$root/scripts/state-map.py" --check "$build" || status=1

exit "$status"
