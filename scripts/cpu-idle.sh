#!/bin/sh
# How much of a host core each corpus program uses while it runs on the
# wall clock and is left alone: each is started headless from a copy of
# its folder in a home of its own, given CPU_SETTLE seconds, and the
# process's CPU time (user and system, every thread) is read over the
# CPU_WINDOW seconds after; then it is ended.  A program waiting for
# input, or paused, should use next to none.
#
# usage: sh scripts/cpu-idle.sh <census dir> <work dir> [bluebox binary] [jobs]
#   The census dir holds corpus/ and census_list.txt, as walk-corpus.sh's.
#   Writes <work dir>/cpu.tsv: program, percent of a core over the
#   window (empty if it wasn't measured), alive or how it ended, the
#   waits --log idle found; and <work dir>/report.txt: the count
#   measured, their sum and how many used more than 5, 20 and 50
#   percent, then the programs, most first.  Each run's output is kept in
#   <work dir>/logs/.
#
#   As it starts (the default): with --realtime, measured from the start.
#   WALK=1: its menus walked first on the instruction clock, measured once
#   the walk is done (BLUEBOX_WALK_STAY: the program goes on, on the wall
#   clock), WALK_TIMEOUT=s (default 300) given for the walk; a program
#   whose walk ended it is not measured.
#   LIST=file reads the programs from file instead of census_list.txt, a
#   line each: the path (under corpus/, or a full one) and optionally,
#   after tabs, K and run flags (e.g. --keys that start a game): the run
#   goes on the instruction clock until K instructions
#   (BLUEBOX_REALTIME_AT), then on the wall clock, measured from there (a
#   game in play, from hibernate-play.tsv's columns).
#   ARGS adds run flags to every run.  CPU_SETTLE=s (default 10),
#   CPU_WINDOW=s (default 10).
set -eu
LC_ALL=C
export LC_ALL

dir=${1:?usage: cpu-idle.sh <census dir> <work dir> [bluebox binary] [jobs]}
work=${2:?usage: cpu-idle.sh <census dir> <work dir> [bluebox binary] [jobs]}
root=$(cd "$(dirname "$0")/.." && pwd)
bin=${3:-${BLUEBOX:-"$(cd "$root" && swift build -c release --show-bin-path)/bluebox"}}
jobs=${4:-4}
settle=${CPU_SETTLE:-10}
window=${CPU_WINDOW:-10}
mkdir -p "$work/logs" "$work/homes"

# A process's CPU time in hundredths of a second (ps: [[h:]m:]s.hh), or
# nothing once it has gone.
cpu_cs() {
	ps -o time= -p "$1" 2>/dev/null | awk -F: '
		NF { s = 0; for (i = 1; i <= NF; i++) s = s * 60 + $i; printf "%d\n", s * 100 + 0.5 }'
}

# How a run ended, from its output ($2 if it doesn't say).
ended() {
	how=$(grep -a -m 1 -E '^stopped:|^instruction limit|^several applications|^bluebox:' "$1" | cut -c1-100 || true)
	printf '%s\n' "${how:-$2}"
}

measure_one() {
	rel=$(printf '%s\n' "$1" | cut -f 1)
	at=$(printf '%s\n' "$1" | cut -s -f 2)
	flags=$(printf '%s\n' "$1" | cut -s -f 3-)
	name=$(printf '%s' "$rel" | tr -c 'A-Za-z0-9._-' '_' | cut -c1-160)_$(printf '%s' "$rel" | cksum | cut -d ' ' -f 1)
	home="$work/homes/$name"
	log="$work/logs/$name.txt"
	case $rel in
	/*) folder=$(dirname "$rel") ;;
	*) folder=$(dirname "$dir/corpus/$rel") ;;
	esac
	if [ ! -d "$folder" ]; then
		printf '%s\t\tnot in corpus/\t0\n' "$rel"
		return
	fi
	rm -rf "$home"
	mkdir -p "$home"
	cp -cR "$folder" "$home/copy" 2>/dev/null || cp -R "$folder" "$home/copy"
	set -- "$home/copy/$(basename "$rel")" --headless --census --max 1000000000000 --log idle
	# What the measurement waits for in the run's output, if anything.
	ready=
	if [ -n "${WALK:-}" ]; then
		set -- "$@" --walk-menus
		ready='^walk: .*, the program goes on'
	elif [ -n "$at" ]; then
		ready='^bluebox: the clock follows real time'
	else
		set -- "$@" --realtime
	fi
	# shellcheck disable=SC2086 # the flags are words
	set -- "$@" ${ARGS:-} $flags
	HOME="$home" BLUEBOX_WALK_STAY=1 BLUEBOX_REALTIME_AT="$at" "$bin" run "$@" >"$log" 2>&1 &
	pid=$!
	# shellcheck disable=SC2064 # this run's process
	trap "kill -KILL $pid 2>/dev/null" EXIT HUP INT TERM
	if [ -n "$ready" ]; then
		t=0
		while kill -0 "$pid" 2>/dev/null && ! grep -a -q -E "$ready" "$log" && [ "$t" -lt "${WALK_TIMEOUT:-300}" ]; do
			sleep 1
			t=$((t + 1))
		done
	fi
	sleep "$settle"
	a=$(cpu_cs "$pid")
	sleep "$window"
	b=$(cpu_cs "$pid")
	pct=
	if [ -n "$ready" ] && ! grep -a -q -E "$ready" "$log"; then
		how=$(ended "$log" "not ready in time")
	elif [ -n "$a" ] && [ -n "$b" ] && kill -0 "$pid" 2>/dev/null; then
		how=alive
		pct=$(awk -v a="$a" -v b="$b" -v w="$window" 'BEGIN { printf "%.1f", (b - a) / w }')
	else
		how=$(ended "$log" ended)
	fi
	kill -KILL "$pid" 2>/dev/null || true
	wait "$pid" 2>/dev/null || true
	waits=$(grep -a -c 'the host sleeps' "$log" || true)
	rm -rf "$home" || true
	printf '%s\t%s\t%s\t%s\n' "$rel" "$pct" "$how" "$waits"
}

if [ $# -ge 5 ]; then
	measure_one "$5"
	exit 0
fi

list=${LIST:-"$dir/census_list.txt"}
grep -v -e '^$' -e '^#' "$list" | while IFS= read -r line; do
	printf '%s\0' "$line"
done | xargs -0 -n 1 -P "$jobs" sh "$0" "$dir" "$work" "$bin" "$jobs" >"$work/cpu.tsv" || true

{
	awk -F'\t' '$2 != "" { n++; s += $2; if ($2 > 5) a++; if ($2 > 20) b++; if ($2 > 50) c++ }
		END { printf "%d measured: sum %.1f%%, above 5%%: %d, above 20%%: %d, above 50%%: %d\n", n, s, a, b, c }' "$work/cpu.tsv"
	sort -t "$(printf '\t')" -k2,2 -rn "$work/cpu.tsv"
} >"$work/report.txt"
