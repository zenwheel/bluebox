#!/bin/sh
# Walk a whole software archive in batches, keeping only the results: each
# batch's archives are copied in, imported (expanded as the census corpus
# is: bluebox import), their applications walked with walk-corpus.sh, and
# everything but the results removed before the next batch.  Resumable:
# batches already done (<work dir>/done.txt) are skipped.
#
# usage: sh scripts/walk-archive.sh <archive folder> <work dir> [jobs] [batch size]
#   An archive over 200 MB (a letter's zip) is a batch of its own; the rest
#   go in batches of <batch size> files (default 200).  jobs defaults to the
#   performance cores.  Results: <work dir>/results/<batch>/ (walk.tsv,
#   report.txt, the logs as logs.tgz, the import notes), and over every
#   batch <work dir>/walk.tsv (the batch, then walk-corpus.sh's columns) and
#   <work dir>/report.txt.  WALK_TIMEOUT and MAX pass through to
#   walk-corpus.sh; HEAPCHECK defaults to 10 here (every 10th call).
#   KEEP, a folder, keeps the expanded applications there as a census
#   corpus (KEEP/corpus/<batch>/..., KEEP/census_list.txt) that
#   walk-corpus.sh can walk again without importing anything.
#   BLUEBOX names the binary to run (default: the release build).
#   REMOUNT, a command, is run when the archive folder has gone (a network
#   share unmounted) before the next batch, and the run stops if it stays
#   gone.
set -eu
LC_ALL=C
export LC_ALL
# unar, which bluebox import runs, where Homebrew puts it.
PATH="$PATH:/opt/homebrew/bin:/usr/local/bin"
export PATH

usage="usage: walk-archive.sh <archive folder> <work dir> [jobs] [batch size]"
src=${1:?"$usage"}
work=${2:?"$usage"}
jobs=${3:-$(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}
size=${4:-200}
root=$(cd "$(dirname "$0")/.." && pwd)
# BLUEBOX, the binary to run; otherwise the release build SwiftPM made.
bin=${BLUEBOX:-"$(cd "$root" && swift build -c release --show-bin-path)/bluebox"}
HEAPCHECK=${HEAPCHECK-10}
export HEAPCHECK
mkdir -p "$work/results"
work=$(cd "$work" && pwd)
touch "$work/done.txt"

# The archive folder there, remounting it if it has gone.
have_source() {
	[ -d "$src" ] && return 0
	[ -n "${REMOUNT:-}" ] && sh -c "$REMOUNT" >/dev/null 2>&1
	sleep 5
	[ -d "$src" ]
}
have_source || { printf '%s: not there\n' "$src" >&2; exit 1; }

# The archives and disk images, each with its batch: "batch<TAB>path".
if [ ! -f "$work/batches.txt" ]; then
	find "$src" -type f ! -name '._*' ! -name '.DS_Store' \( -iname '*.zip' -o -iname '*.sit' \
		-o -iname '*.sea' -o -iname '*.hqx' -o -iname '*.bin' -o -iname '*.img' -o -iname '*.image' \
		-o -iname '*.dsk' -o -iname '*.cpt' -o -iname '*.smi' -o -iname '*.dmg' -o -iname '*.toast' \
		-o -iname '*.iso' -o -iname '*.sitx' \) | sort | while IFS= read -r f; do
		printf '%s\t%s\n' "$(stat -f %z "$f" 2>/dev/null || stat -c %s "$f")" "$f"
	done | awk -F'\t' -v size="$size" '
		$1 > 200000000 { n = $2; sub(/.*\//, "", n); gsub(/[^A-Za-z0-9._-]/, "_", n); print n "\t" $2; next }
		{ if (k % size == 0) b = sprintf("batch-%04d", k / size + 1); k++; print b "\t" $2 }' >"$work/batches.txt"
fi

cut -f1 "$work/batches.txt" | uniq | while IFS= read -r b; do
	if grep -qx "$b" "$work/done.txt"; then continue; fi
	have_source || { printf '%s: gone, stopping\n' "$src" >&2; exit 1; }
	printf '%s: ' "$b"
	t=$work/batch
	rm -rf "$t"
	mkdir -p "$t/in" "$t/census" "$t/h" "$work/results/$b"
	if [ -n "${KEEP:-}" ]; then
		rm -rf "${KEEP:?}/corpus/$b"
		mkdir -p "$KEEP/corpus/$b"
		ln -s "$KEEP/corpus/$b" "$t/census/corpus"
	else
		mkdir -p "$t/census/corpus"
	fi
	# Copy each archive in, import it, and drop the copy.
	awk -F'\t' -v b="$b" '$1 == b { print $2 }' "$work/batches.txt" | while IFS= read -r f; do
		c="$t/in/$(basename "$f")"
		cp "$f" "$c" 2>/dev/null || { printf '%s: not copied\n' "$f" >>"$work/results/$b/import.txt"; continue; }
		HOME="$t/h" "$bin" import "$c" --library "$t/census/corpus" >>"$work/results/$b/import.txt" 2>&1 || true
		rm -f "$c"
	done
	# The applications found, as walk-corpus.sh wants them: paths under corpus/.
	HOME="$t/h" "$bin" apps --library "$t/census/corpus" 2>/dev/null |
		LC_ALL=en_US.UTF-8 sed -n -E 's/^.{8} .{4}  //p' | grep -v '^Macintosh HD/' >"$t/census/census_list.txt" || true
	n=$(wc -l <"$t/census/census_list.txt" | tr -d ' ')
	printf '%s applications\n' "$n"
	if [ "$n" -gt 0 ]; then
		sh "$root/scripts/walk-corpus.sh" "$t/census" "$t/out" "$bin" "$jobs" >"$work/results/$b/summary.txt" 2>&1 || true
		cp "$t/out/walk.tsv" "$t/out/report.txt" "$work/results/$b/" 2>/dev/null || true
		tar czf "$work/results/$b/logs.tgz" -C "$t/out" logs 2>/dev/null || true
		awk -v b="$b" '{ print b "\t" $0 }' "$t/out/walk.tsv" >>"$work/walk.tsv" 2>/dev/null || true
	fi
	if [ -n "${KEEP:-}" ]; then
		sed "s|^|$b/|" "$t/census/census_list.txt" >>"$KEEP/census_list.txt"
	fi
	printf '%s\n' "$b" >>"$work/done.txt"
	rm -rf "$t"
done

# The failures by kind over every batch, as walk-corpus.sh counts them.
cut -f3 "$work/walk.tsv" 2>/dev/null | sed -E 's/^(host memory error: [^ ]+ [^ ]+).*/\1/; s/^(heap damaged by [^ ]+).*/\1/; s/^(code changed by [^ ]+).*/\1/; s/^(stopped: unimplemented: [^ ]+).*/\1/; s/^(stopped: library not found: .*) \(for [^)]*\).*/\1/; s/^(stopped: menu walk stuck in a dialog).*/\1/; s/ after [0-9]+ instructions//; s/\(pc [0-9A-F]+\)//; s/^(bluebox: [^/]*).*/\1/' |
	grep -v -E 'menu walk finished|waiting for input|not in corpus' | sort | uniq -c | sort -rn >"$work/report.txt" || true
touch "$work/walk.tsv"
printf '%s programs, %s finished their walk\n' "$(wc -l <"$work/walk.tsv" | tr -d ' ')" \
	"$(grep -c 'menu walk finished' "$work/walk.tsv" || true)"
