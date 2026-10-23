#!/bin/sh
# Walk every corpus program's menus (bluebox run --walk-menus) with the
# heaps checked after every Toolbox call, several at a time, each in a
# home of its own and from a copy of its folder (the walk saves and
# exports; the corpus is never written), and rank what went wrong.  A
# binary built with AddressSanitizer (swift build --sanitize=address)
# also catches Blue Box's own memory errors.
#
# usage: sh scripts/walk-corpus.sh <census dir> <work dir> [bluebox binary] [jobs]
#   The census dir holds corpus/ and census_list.txt (paths under corpus/,
#   one a line).  Writes <work dir>/walk.tsv (program, how it ended, the
#   last step, items chosen) and <work dir>/report.txt (the failures by
#   kind, most first); each run's output is kept in <work dir>/logs/.
#   HEAPCHECK=n checks the heaps every nth call (default 1; 0 for none),
#   CODECHECK=n checks a PowerPC program's code for changes every nth
#   call (default 0, none),
#   WALK_TIMEOUT=s gives each run s seconds (default 600), MAX=n instructions
#   (default 4000000000), NOWALK=1 leaves the programs alone (no walk).
set -eu
LC_ALL=C
export LC_ALL

dir=${1:?usage: walk-corpus.sh <census dir> <work dir> [bluebox binary] [jobs]}
work=${2:?usage: walk-corpus.sh <census dir> <work dir> [bluebox binary] [jobs]}
root=$(cd "$(dirname "$0")/.." && pwd)
bin=${3:-"$root/.build/release/bluebox"}
jobs=${4:-8}
mkdir -p "$work/logs" "$work/homes"

# One program: its line of walk.tsv.  With a second argument, that one of
# the applications an archive holds (--app).
walk_one() {
	rel=$1
	app=${2:-}
	name=$(printf '%s%s' "$rel" "${app:+ $app}" | tr '/ ' '__')
	home="$work/homes/$name"
	log="$work/logs/$name.txt"
	rm -rf "$home"
	mkdir -p "$home"
	# A clone where the file system makes them (APFS: no space, no time).
	folder=$(dirname "$dir/corpus/$rel")
	if [ ! -d "$folder" ]; then
		printf '%s\tnot in corpus/ (inside an archive)\t\t0\n' "$rel"
		rm -rf "$home"
		return
	fi
	cp -cR "$folder" "$home/copy" 2>/dev/null || cp -R "$folder" "$home/copy"
	status=0
	# NOWALK: the program left alone, to tell its own quits from the walk's.
	walk=--walk-menus
	[ -z "${NOWALK:-}" ] || walk=--census
	set -- "$home/copy/$(basename "$rel")" --headless --census "$walk" --max "${MAX:-4000000000}"
	[ -z "$app" ] || set -- "$@" --app "$app"
	HOME="$home" BLUEBOX_HEAPCHECK="${HEAPCHECK-1}" BLUEBOX_CODECHECK="${CODECHECK-0}" ASAN_OPTIONS=detect_leaks=0 timeout "${WALK_TIMEOUT:-600}" \
		"$bin" run "$@" >"$log" 2>&1 || status=$?
	rm -rf "$home"
	if grep -q 'ERROR: AddressSanitizer' "$log"; then
		end="host memory error: $(grep -m 1 'ERROR: AddressSanitizer' "$log" | sed 's/.*AddressSanitizer: //' | cut -c1-80)"
	elif [ "$status" -ge 128 ] || [ "$status" -eq 124 ]; then
		end="host exit $status"
	else
		end=$(grep -a -m 1 -E '^stopped:|^bluebox:|^instruction limit|^several applications' "$log" | sed -E 's/ after [0-9]+ instructions//; s/\(pc [0-9A-F]+\)//' | cut -c1-120)
	fi
	# An archive of several applications: each of them in turn.
	if [ -z "$app" ] && grep -q '^several applications' "$log"; then
		sed -n '/^several applications/,$ s/^  //p' "$log" | while IFS= read -r one; do
			walk_one "$rel" "$one"
		done
		return
	fi
	step=$(grep -a '^walk: menu' "$log" | tail -n 1 | sed 's/^walk: //')
	items=$(grep -a -c '^walk: menu' "$log" || true)
	printf '%s\t%s\t%s\t%s\n' "$rel${app:+ [$app]}" "${end:-none}" "${step:-}" "$items"
}

if [ "${WALK_ONE:-}" ]; then
	walk_one "$WALK_ONE"
	exit 0
fi

grep -v '^$' "$dir/census_list.txt" | while IFS= read -r rel; do
	printf '%s\0' "$rel"
done | xargs -0 -P "$jobs" -I {} env WALK_ONE={} sh "$0" "$dir" "$work" "$bin" "$jobs" >"$work/walk.tsv" || true

# The failures by kind: host errors, damaged heaps or code, missing routines,
# CPU faults, then the rest; walks that finished or reached the limit
# waiting for input are fine.
cut -f2 "$work/walk.tsv" | sed -E 's/^(host memory error: [^ ]+ [^ ]+).*/\1/; s/^(heap damaged by [^ ]+).*/\1/; s/^(code changed by [^ ]+).*/\1/; s/^(stopped: unimplemented: [^ ]+).*/\1/; s/^(stopped: menu walk stuck in a dialog).*/\1/' |
	grep -v -E 'menu walk finished|waiting for input|not in corpus' | sort | uniq -c | sort -rn >"$work/report.txt" || true
printf '%s programs, %s finished their walk\n' "$(wc -l <"$work/walk.tsv" | tr -d ' ')" \
	"$(grep -c 'menu walk finished' "$work/walk.tsv" || true)"
