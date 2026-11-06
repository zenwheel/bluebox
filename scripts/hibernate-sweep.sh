#!/bin/sh
# Hibernate every corpus program and resume it in a new process, several
# at a time, each in a home of its own and from a copy of its folder (the
# corpus is never written), and rank what went wrong.  For each program:
#
#   H  runs K instructions and on to the next safe point (the program's
#      event loop, drain.c), saves its snapshot and ends;
#   A  runs again from the folder and home as H found them, hibernates at
#      the same point and goes on in the same process (--then), its menus
#      walked from there: the reference;
#   B  resumes H's snapshot in a new process, with the folder and home as
#      H left them, and walks the menus.
#
# A resumed walk passes when it ends as A's does (the same stop, without
# its instruction count), with the same number of steps and the same last
# step.  Those that pass are swept again at a later K (K2).
#
# usage: sh scripts/hibernate-sweep.sh <census dir> <work dir> [bluebox binary] [jobs]
#   The census dir holds corpus/ and census_list.txt, as walk-corpus.sh's
#   (LIST=file reads the paths from file instead).  Writes <work dir>/sweep.tsv:
#     program, K, hibernate (saved / refused / failed / not hibernated),
#     why (the refusal's reason, or where it stopped), snapshot bytes,
#     resume (ok / crashed / exit / timed out), A's end, B's end, A's steps,
#     B's steps, A's last step, B's last step, walk (same / same end /
#     differ: what), seconds for H, A and B, and the class (pass, expected
#     refusal, sound, unexpected refusal, not hibernated, save failure,
#     resume crash, divergent walk, both timed out)
#   and <work dir>/report.txt (the totals by class, refusals by reason);
#   each run's output is kept in <work dir>/logs/<program>/.
#   K=n hibernates after n instructions (default 30000000), K2=n the second
#   pass (default 300000000; 0 for none), MAX=n instructions for each walk
#   (default 24000000000), WALK_TIMEOUT=s seconds for each run (default
#   600), HEAPCHECK=n as walk-corpus.sh's (default 0), KEEP=1 keeps every
#   snapshot (otherwise only those of programs that did not pass).
set -eu
LC_ALL=C
export LC_ALL
# A snapshot deleted goes to the Trash otherwise: never the real user's.
BLUEBOX_NO_TRASH=1
export BLUEBOX_NO_TRASH
PATH="$PATH:/opt/homebrew/bin:/usr/local/bin"
export PATH

dir=${1:?usage: hibernate-sweep.sh <census dir> <work dir> [bluebox binary] [jobs]}
work=${2:?usage: hibernate-sweep.sh <census dir> <work dir> [bluebox binary] [jobs]}
root=$(cd "$(dirname "$0")/.." && pwd)
bin=${3:-${BLUEBOX:-"$(cd "$root" && swift build -c release --show-bin-path)/bluebox"}}
jobs=${4:-8}
K=${K:-30000000}
K2=${K2:-300000000}
mkdir -p "$work/logs" "$work/runs"

# The first line of a run's output saying how it stopped, without the
# instruction count and program counter (they say where, not how).
end_of() {
	e=$(grep -a -m 1 -E '^stopped:|^instruction limit' "$1" || true)
	[ -n "$e" ] || e=$(grep -a -m 1 '^bluebox:' "$1" || true)
	printf '%s\n' "$e" | sed -E 's/ after [0-9]+ instructions//; s/ ?\(pc [0-9A-F]+\)//' | cut -c1-120
}

# A walk's steps (as walk-corpus.sh counts them) into a file of its own.
steps_of() {
	grep -a -E '^walk: (menu|button|target|key)' "$1" >"$2" || true
}

# How a run ended when it did not end by itself.
host_end() {
	case $1 in
	124) printf 'timed out' ;;
	*) if [ "$1" -ge 128 ]; then printf 'crashed (signal %d)' $(($1 - 128)); fi ;;
	esac
}

# Seconds since the epoch.
now() { date +%s; }

sweep_one() {
	rel=$1
	k=$2
	name=$(printf '%s' "$rel" | tr '/ ' '__')
	if [ "$(printf '%s' "$name" | wc -c)" -gt 200 ]; then
		name=$(printf '%s' "$name" | tr -c 'A-Za-z0-9._-' '_' | cut -c1-160)_$(printf '%s' "$name" | cksum | cut -d ' ' -f 1)
	fi
	name=${name}_k$k
	w=$work/runs/$name
	logs=$work/logs/$name
	folder=$(dirname "$dir/corpus/$rel")
	if [ ! -d "$folder" ]; then
		printf '%s\t%s\tnot hibernated\tnot in corpus/\t\t\t\t\t\t\t\t\t\t\t\t\tnot hibernated\n' "$rel" "$k"
		return
	fi
	rm -rf "$w" "$logs"
	mkdir -p "$w/orig" "$w/h" "$logs"
	d=$(basename "$folder")
	cp -cR "$folder" "$w/orig/$d" 2>/dev/null || cp -R "$folder" "$w/orig/$d"
	cp -cR "$w/orig/$d" "$w/$d"
	app=$w/$d/$(basename "$rel")
	max=$((k + 400000000))
	limit=${WALK_TIMEOUT:-600}
	hc=${HEAPCHECK-0}

	# H: to K, on to a safe point, saved; the walk starts (as A's does)
	# only once it has hibernated, and the run ends straight after.
	t0=$(now)
	st=0
	HOME=$w/h BLUEBOX_HEAPCHECK="$hc" timeout "$limit" "$bin" run "$app" --headless --census --walk-menus \
		--max "$max" --hibernate-at "$k" --hibernate-to "$w/k.bbhib" --then 1 >"$logs/h.out" 2>"$logs/h.err" || st=$?
	th=$(($(now) - t0))
	bytes='' why='' resume='' aend='' bend='' an='' bn='' alast='' blast='' walk='' ta='' tb=''
	line=$(grep -a -m 1 -E '^(hibernated|hibernate refused|hibernate failed)' "$logs/h.out" || true)
	case $line in
	hibernated:*)
		hib=saved
		bytes=$(printf '%s' "$line" | sed -E 's/.*\(([0-9]+) bytes.*/\1/')
		why=$(printf '%s' "$line" | sed -E 's/.*stopped at (.*) after [0-9]+ instructions\)$/\1/')
		;;
	"hibernate refused:"*)
		hib=refused
		why=${line#hibernate refused: }
		;;
	"hibernate failed:"*)
		hib=failed
		why=${line#hibernate failed: }
		;;
	*)
		hib="not hibernated"
		why=$(host_end "$st")
		[ -n "$why" ] || why=$(end_of "$logs/h.out")
		;;
	esac
	[ -f "$w/k.bbhib" ] || [ "$hib" != saved ] || { hib=failed why="no snapshot written"; }

	if [ "$hib" = saved ]; then
		# A: from the folder and home as H found them.
		mv "$w/$d" "$w/$d.at"
		mv "$w/h" "$w/h.at"
		cp -cR "$w/orig/$d" "$w/$d"
		mkdir -p "$w/h"
		t0=$(now)
		st=0
		HOME=$w/h BLUEBOX_HEAPCHECK="$hc" timeout "$limit" "$bin" run "$app" --headless --census --walk-menus \
			--max "$max" --hibernate-at "$k" --hibernate-to "$w/k2.bbhib" --then "${MAX:-24000000000}" \
			>"$logs/a.out" 2>"$logs/a.err" || st=$?
		ta=$(($(now) - t0))
		aend=$(host_end "$st")
		[ -n "$aend" ] || aend=$(end_of "$logs/a.out")
		# The reference hibernated where H did, or it is no reference.
		ahib=$(grep -a -m 1 -E '^(hibernated|hibernate refused|hibernate failed)' "$logs/a.out" |
			sed -E 's/.*stopped at (.*) after ([0-9]+) instructions\)$/\1 \2/' || true)
		hhib=$(printf '%s' "$line" | sed -E 's/.*stopped at (.*) after ([0-9]+) instructions\)$/\1 \2/')
		rm -rf "${w:?}/$d" "$w/h" "$w/k2.bbhib"
		mv "$w/$d.at" "$w/$d"
		mv "$w/h.at" "$w/h"

		# B: H's snapshot resumed, with the folder and home as H left them.
		t0=$(now)
		st=0
		HOME=$w/h BLUEBOX_HEAPCHECK="$hc" BLUEBOX_CRASH_TRACE=1 timeout "$limit" "$bin" resume "$w/k.bbhib" \
			--headless --census --walk-menus --max "${MAX:-24000000000}" >"$logs/b.out" 2>"$logs/b.err" || st=$?
		tb=$(($(now) - t0))
		bend=$(host_end "$st")
		if [ -n "$bend" ]; then
			resume=$bend
		elif ! grep -a -q '^resumed: ' "$logs/b.out"; then
			resume="not resumed: $(grep -a -m 1 -E '^bluebox:|error' "$logs/b.out" "$logs/b.err" | cut -c1-120 || true)"
			bend=$(end_of "$logs/b.out")
		else
			resume=ok
			bend=$(end_of "$logs/b.out")
		fi
		steps_of "$logs/a.out" "$logs/a.walk"
		steps_of "$logs/b.out" "$logs/b.walk"
		an=$(wc -l <"$logs/a.walk" | tr -d ' ')
		bn=$(wc -l <"$logs/b.walk" | tr -d ' ')
		alast=$(tail -n 1 "$logs/a.walk" | sed 's/^walk: //' | tr -d '\t' | cut -c1-80)
		blast=$(tail -n 1 "$logs/b.walk" | sed 's/^walk: //' | tr -d '\t' | cut -c1-80)
		if [ "$aend" != "$bend" ]; then
			walk="differ: end"
		elif [ "$an" != "$bn" ]; then
			walk="differ: steps"
		elif [ "$alast" != "$blast" ]; then
			walk="differ: last step"
		elif cmp -s "$logs/a.walk" "$logs/b.walk" && [ "$(grep -a -m 1 -E '^stopped:|^instruction limit' "$logs/a.out")" = \
			"$(grep -a -m 1 -E '^stopped:|^instruction limit' "$logs/b.out")" ]; then
			walk=same
		else
			walk="same end"
		fi
		# A program that reads the date (or the host's time) may come to
		# its safe point elsewhere in A: noted, as A is then a reference
		# only for how the walk ends.
		[ "$ahib" = "$hhib" ] || walk="$walk (A hibernated at $ahib, H at $hhib)"
	fi

	class=$(classify "$hib" "$why" "$resume" "$walk" "$aend")
	[ -z "${KEEP:-}" ] && [ "$class" = pass ] && rm -f "$w/k.bbhib"
	if [ -f "$w/k.bbhib" ]; then mv "$w/k.bbhib" "$logs/k.bbhib"; fi
	rm -rf "$w"
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$rel" "$k" "$hib" "$why" "$bytes" \
		"$resume" "$aend" "$bend" "$an" "$bn" "$alast" "$blast" "$walk" "$th" "$ta" "$tb" "$class"
}

# The class of a program's sweep: what went wrong first.
classify() {
	case $1 in
	saved) ;;
	refused)
		case $2 in
		*speaking* | *"recording sound"* | *[Ss]ound* | *[Ss]peech* | *MIDI* | *[Mm]usic*) echo sound ;;
		*MacTCP* | *"Open Transport"* | *AppleTalk* | *AppleShare* | *"PPC Toolbox"* | *"serial port"* | \
			*"Communications Toolbox"* | *printing* | *OpenGL* | *RAVE* | *"QuickDraw 3D"* | *movie* | \
			*"disk image"* | *"Multiprocessing Services"*) echo "expected refusal" ;;
		*) echo "unexpected refusal" ;;
		esac
		return
		;;
	failed)
		echo "save failure"
		return
		;;
	*)
		echo "not hibernated"
		return
		;;
	esac
	if [ "$3" = "timed out" ] && [ "$5" = "timed out" ]; then
		echo "both timed out" # a walk too long for WALK_TIMEOUT, resumed or not
	elif [ "$3" != ok ]; then
		echo "resume crash"
	else
		case $4 in
		same*) echo pass ;;
		*) echo "divergent walk" ;;
		esac
	fi
}

# One program at one K, the sixth and fifth arguments: what each of the
# parallel runs below does.
if [ $# -ge 6 ]; then
	sweep_one "$6" "$5"
	exit 0
fi

list=${LIST:-$dir/census_list.txt}
pass() {
	grep -v '^$' "$1" | while IFS= read -r rel; do
		printf '%s\0' "$rel"
	done | xargs -0 -n 1 -P "$jobs" sh "$0" "$dir" "$work" "$bin" "$jobs" "$2" || true
}
pass "$list" "$K" >"$work/sweep.tsv"
if [ "$K2" -gt 0 ]; then
	awk -F '\t' '$17 == "pass" { print $1 }' "$work/sweep.tsv" >"$work/pass2.txt"
	pass "$work/pass2.txt" "$K2" >>"$work/sweep.tsv"
fi

# The totals by class for each K, then the refusals by reason.
{
	cut -f2 "$work/sweep.tsv" | sort -nu | while IFS= read -r k; do
		printf 'K %s: %s programs\n' "$k" "$(awk -F '\t' -v k="$k" '$2 == k' "$work/sweep.tsv" | wc -l | tr -d ' ')"
		awk -F '\t' -v k="$k" '$2 == k { print $17 }' "$work/sweep.tsv" | sort | uniq -c | sort -rn
		printf '  refused, by reason:\n'
		awk -F '\t' -v k="$k" '$2 == k && $3 == "refused" { print $4 }' "$work/sweep.tsv" |
			sed -E 's/thread [0-9]+ /thread N /; s/\(pc [0-9A-F]+\)//' | sort | uniq -c | sort -rn
		printf '\n'
	done
} >"$work/report.txt"
cat "$work/report.txt"
