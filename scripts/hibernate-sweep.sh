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
#
# PLAY=1 sweeps games in play instead (PLAY=file: another table than
# scripts/hibernate-play.tsv beside this script).  Each row there gives a
# program, the K to hibernate it at, the scripted input (--keys) that
# starts a game before K and has it busy at K (keys held, sound playing,
# its timers and VBL tasks running), the input to give it once resumed,
# and run flags.  For each:
#
#   H  plays to K and on to the next safe point, saves and ends;
#   B  resumes H's snapshot in a new process (folder and home as H left
#      them) and runs on to K + AFTER (default 300000000), its screen saved
#      at K + MID (default 20000000), K + MID + WIN (default 100000000) and
#      just before the end, the input after the resume given after the
#      first;
#   B0 the same without that input (a sync step in place of each);
#   C  plays from a fresh folder and home, never hibernating, with the
#      same input at the same counts and its screens saved at them;
#   C2 C again, which says whether the game plays the same each time.
# Each run is given the whole script (H's part after K never comes): a
# program idle in GetNextEvent has its clock move on to the next input, so
# input to come changes how a run goes before it.  From K on the script
# has a sync step every GRID instructions (default 1000000), so that no
# run's clock jumps further than that, the drain's included.  The table's
# keys may use pace:N for the same before K.  Every run starts at
# the same date (DATE, default 2000-01-01T12:00:00), as programs seed
# their random numbers with it.
#
# Writes <work dir>/play.tsv: program, K, hibernate, why, snapshot bytes,
# resume, B's end, animates (B0's first and last screens differ), input
# (B's second screen differs from B0's), deterministic (C's screens are
# C2's), match (B's screens are C's, or the first that differs), seconds
# for H, B and C, and the class (pass, pass but not deterministic,
# refused, not hibernated, save failure, resume crash, stopped after
# resume, input ignored, differs from unhibernated); and
# <work dir>/play-report.txt.  Its screens are kept in <work dir>/logs.
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

# A program's work folder's name, from its path.
name_of() {
	name=$(printf '%s' "$1" | tr '/ ' '__')
	# Paths the mapping makes the same ("interfazed 680x0/..." and
	# "interfazed_680x0/...") would share a work folder, and parallel runs
	# delete each other's: kept apart by the path's checksum.
	if [ -f "$work/ambiguous.txt" ] && grep -Fqx -- "$name" "$work/ambiguous.txt"; then
		name=${name}_$(printf '%s' "$1" | cksum | cut -d ' ' -f 1)
	fi
	if [ "$(printf '%s' "$name" | wc -c)" -gt 200 ]; then
		name=$(printf '%s' "$name" | tr -c 'A-Za-z0-9._-' '_' | cut -c1-160)_$(printf '%s' "$name" | cksum | cut -d ' ' -f 1)
	fi
	printf '%s' "$name"
}

sweep_one() {
	rel=$1
	k=$2
	name=$(name_of "$rel")_k$k
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

# The same screen: the two files are there and alike.
same_screen() { [ -f "$1" ] && [ -f "$2" ] && cmp -s "$1" "$2"; }

# The part of a play's script after K, for a run whose script starts at
# instruction $1 (0, or where a resumed one resumed): its screens saved as
# $2-mid.png, $2-post.png and $2-end.png, the input $3 given after the
# first.
screens() {
	printf 'pace:0,at:%s,snap:%s-mid.png,%s%sat:%s,snap:%s-post.png,at:%s,snap:%s-end.png,%s' \
		"$(since "$1" $((k + mid)))" "$2" "$3" "${3:+,}" "$(since "$1" $((k + mid + win)))" "$2" \
		"$(since "$1" $((k + after - 1000000)))" "$2" "$(grid "$1")"
}

# Sync steps every GRID instructions from K on (default 1000000), for a
# run whose script starts at $1: a program idle in GetNextEvent has its
# clock move on no further than the next, in every run alike, wherever
# the input after the resume is (and a drain, which does not stop at the
# polls some games make, runs on no further either).
grid() {
	awk -v s="$1" -v k="$k" -v g="${GRID:-1000000}" -v n="$after" 'BEGIN {
		out = ""
		for (t = k + g; t < k + n; t += g)
			if (t > s)
				out = out (out == "" ? "" : ",") "at:" (t - s) ",sync"
		print out
	}'
}

# A run's script ($2), kept as <logs>/$1.keys too (to run it again by hand).
keys_for() {
	printf '%s\n' "$2" >"$logs/$1.keys"
	printf '%s' "$2"
}

# Instruction $2 counted from $1 (0 when $1 is past it).
since() { if [ "$2" -gt "$1" ]; then echo $(($2 - $1)); else echo 0; fi; }

# A script's input steps made sync steps: the same points for the clock,
# no input.
no_input() {
	printf '%s' "$1" | tr ',' '\n' | sed -E '/^(wait|at|pace|snap):|^sync$|^$/!s/.*/sync/' | paste -s -d , -
}

# One game of the PLAY table, played, hibernated and resumed (see the top).
play_one() {
	rel=$1
	row=$(awk -F '\t' -v p="$rel" '$1 == p { print; exit }' "$table")
	k=$(printf '%s' "$row" | cut -f2)
	keys=$(printf '%s' "$row" | cut -f3)
	post=$(printf '%s' "$row" | cut -f4)
	flags=$(printf '%s' "$row" | cut -f5)
	after=${AFTER:-300000000}
	mid=${MID:-20000000}
	win=${WIN:-100000000}
	date=${DATE:-2000-01-01T12:00:00}
	limit=${WALK_TIMEOUT:-600}
	name=$(name_of "$rel")
	w=$work/runs/$name
	logs=$work/logs/$name
	case $rel in
	/*) folder=$(dirname "$rel") ;;
	*) folder=$(dirname "$dir/corpus/$rel") ;;
	esac
	if [ ! -d "$folder" ]; then
		printf '%s\t%s\tnot hibernated\tnot in corpus/\t\t\t\t\t\t\t\t\t\t\tnot hibernated\n' "$rel" "$k"
		return
	fi
	rm -rf "$w" "$logs"
	mkdir -p "$w/orig" "$w/h" "$logs"
	d=$(basename "$folder")
	cp -cR "$folder" "$w/orig/$d" 2>/dev/null || cp -R "$folder" "$w/orig/$d"
	cp -cR "$w/orig/$d" "$w/$d"
	app=$w/$d/$(basename "$rel")

	# H: played to K, saved.
	t0=$(now)
	st=0
	# shellcheck disable=SC2086 # the table's flags are words
	HOME=$w/h timeout "$limit" "$bin" run "$app" --headless --max $((k + 400000000)) \
		--keys "$(keys_for h "$keys,$(screens 0 "$logs/h" "$post")")" --hibernate-at "$k" --hibernate-to "$w/k.bbhib" --then 1 --date "$date" $flags >"$logs/h.out" 2>"$logs/h.err" || st=$?
	th=$(($(now) - t0))
	bytes='' why='' resume='' bend='' anim='' input='' det='' match='' tb='' tc=''
	line=$(grep -a -m 1 -E '^(hibernated|hibernate refused|hibernate failed)' "$logs/h.out" || true)
	case $line in
	hibernated:*)
		hib=saved
		bytes=$(printf '%s' "$line" | sed -E 's/.*\(([0-9]+) bytes.*/\1/')
		why=$(printf '%s' "$line" | sed -E 's/.*stopped at (.*) after [0-9]+ instructions\)$/\1/')
		n=$(printf '%s' "$line" | sed -E 's/.* after ([0-9]+) instructions\)$/\1/')
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
		# The folder and home as H left them, for each resume.
		mv "$w/$d" "$w/$d.h"
		mv "$w/h" "$w/h.h"
		t0=$(now)
		for r in b b0; do
			rm -rf "${w:?}/$d" "$w/h"
			cp -cR "$w/$d.h" "$w/$d"
			cp -cR "$w/h.h" "$w/h"
			p=$post
			[ "$r" = b ] || p=$(no_input "$post")
			st=0
			HOME=$w/h BLUEBOX_CRASH_TRACE=1 timeout "$limit" "$bin" resume "$w/k.bbhib" --headless \
				--max $((k + after - n)) --keys "$(keys_for "$r" "$(screens "$n" "$logs/$r" "$p")")" >"$logs/$r.out" 2>"$logs/$r.err" || st=$?
			[ "$r" = b ] && bst=$st
		done
		tb=$(($(now) - t0))
		bend=$(host_end "$bst")
		if [ -n "$bend" ]; then
			resume=$bend
		elif ! grep -a -q '^resumed: ' "$logs/b.out"; then
			resume="not resumed: $(grep -a -m 1 -E '^bluebox:|error' "$logs/b.out" "$logs/b.err" | cut -c1-120 || true)"
			bend=$(end_of "$logs/b.out")
		else
			resume=ok
			bend=$(end_of "$logs/b.out")
		fi
		if same_screen "$logs/b0-mid.png" "$logs/b0-end.png"; then anim=still; else anim=animates; fi
		if [ -z "$post" ]; then
			input='none given'
		elif same_screen "$logs/b-post.png" "$logs/b0-post.png"; then
			input='no change'
		else
			input='changes screen'
		fi

		# C and C2: never hibernated, the same input at the same counts.
		t0=$(now)
		for r in c c2; do
			rm -rf "${w:?}/$d" "$w/h"
			cp -cR "$w/orig/$d" "$w/$d"
			mkdir -p "$w/h"
			# shellcheck disable=SC2086 # the table's flags are words
			HOME=$w/h timeout "$limit" "$bin" run "$app" --headless --max $((k + after)) \
				--keys "$(keys_for "$r" "$keys,$(screens 0 "$logs/$r" "$post")")" --date "$date" $flags >"$logs/$r.out" 2>"$logs/$r.err" || true
		done
		tc=$(($(now) - t0))
		det=yes
		match=same
		for s in mid post end; do
			same_screen "$logs/c-$s.png" "$logs/c2-$s.png" || det=no
			[ "$match" != same ] || same_screen "$logs/b-$s.png" "$logs/c-$s.png" || match="differ from $s"
		done
	fi

	if [ "$hib" = refused ]; then
		class=refused
	elif [ "$hib" = failed ]; then
		class='save failure'
	elif [ "$hib" != saved ]; then
		class='not hibernated'
	elif [ "$resume" != ok ]; then
		class='resume crash'
	elif ! printf '%s' "$bend" | grep -q 'instruction limit'; then
		class='stopped after resume'
	elif [ "$input" = 'no change' ]; then
		class='input ignored'
	elif [ "$det" = no ]; then
		class='pass but not deterministic'
	elif [ "$match" != same ]; then
		class='differs from unhibernated'
	else
		class=pass
	fi
	# What a resume needs (the snapshot, the folder and home as H left
	# them, at their paths) kept when it did not pass.
	if [ -n "${KEEP:-}" ] || [ "${class%% *}" != pass ]; then
		rm -rf "${w:?}/$d" "$w/h" "$w/orig"
	else
		rm -rf "$w"
	fi
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$rel" "$k" "$hib" "$why" "$bytes" \
		"$resume" "$bend" "$anim" "$input" "$det" "$match" "$th" "$tb" "$tc" "$class"
}

table=${PLAY:-}
[ "$table" != 1 ] || table=$root/scripts/hibernate-play.tsv

# One program at one K, the sixth and fifth arguments (or one game, with
# play the fifth): what each of the parallel runs below does.
if [ $# -ge 6 ] && [ "$5" = play ]; then
	play_one "$6"
	exit 0
fi
if [ $# -ge 6 ]; then
	sweep_one "$6" "$5"
	exit 0
fi

if [ -n "$table" ]; then
	# The table's programs (or LIST's, those of them in it).
	list=${LIST:-$work/play-list.txt}
	[ -n "${LIST:-}" ] || grep -v -e '^#' -e '^$' "$table" | cut -f1 >"$list"
else
	list=${LIST:-$dir/census_list.txt}
fi
# The names two or more paths map to (see sweep_one).
grep -v '^$' "$list" | tr '/ ' '__' | sort | uniq -d >"$work/ambiguous.txt"
pass() {
	grep -v '^$' "$1" | while IFS= read -r rel; do
		printf '%s\0' "$rel"
	done | xargs -0 -n 1 -P "$jobs" sh "$0" "$dir" "$work" "$bin" "$jobs" "$2" || true
}
if [ -n "$table" ]; then
	pass "$list" play >"$work/play.tsv"
	{
		printf '%s games\n' "$(wc -l <"$work/play.tsv" | tr -d ' ')"
		cut -f15 "$work/play.tsv" | sort | uniq -c | sort -rn
	} >"$work/play-report.txt"
	cat "$work/play-report.txt"
	exit 0
fi
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
