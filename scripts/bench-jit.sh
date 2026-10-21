#!/bin/sh
# The JIT's benchmark: a few busy corpus programs run headless for a fixed
# number of instructions (games, and for PowerPC two of Sigma Chess's
# endgame generators, which compute without stopping; a game waiting for
# input skips its idle time, so the Toolbox weighs more there), by the interpreter and with --jit (first with an
# empty translation cache, then with the one that run left), timed one at a
# time.  Prints the seconds and millions of instructions per second of
# each, the JIT's speed-up, and whether each JIT run ended in the same
# state as the interpreter's (the same instruction count, pc and
# registers).  Some programs aren't repeatable from run to run (they seed
# from the date): those differ whatever runs them.
#
# usage: sh scripts/bench-jit.sh <census dir> <work dir> [bluebox binary] [instructions]
#   The census dir holds corpus/; the work dir gets a home the archives
#   are added to by a first, untimed run, a fresh copy of it for each timed
#   run (programs keep preferences there), and the translation caches.
set -eu

dir=${1:?usage: bench-jit.sh <census dir> <work dir> [bluebox binary] [instructions]}
work=${2:?usage: bench-jit.sh <census dir> <work dir> [bluebox binary] [instructions]}
root=$(cd "$(dirname "$0")/.." && pwd)
bin=${3:-"$root/.build/release/bluebox"}
max=${4:-600000000}
games="$dir/corpus/shareware_games_s.zip/shareware_games_s"

# Architecture, then the path below the games folder.
apps='68k	Squirt Derby!.sit ƒ/Squirt Derby!/Squirt Derby!
68k	Shards 1.1 archive.sit ƒ/Shards 1.1 archive/Shards
68k	Safari 1.0.sit ƒ/Safari 1.0/Safari 1.0
ppc	Squirt Derby!.sit ƒ/Squirt Derby!/Squirt Derby!
ppc	Sigma Chess 5.1.3 Shareware.sit ƒ/Sigma Chess 5.1.3 Shareware ƒ/∑ Chess 5.1.3 ƒ/Endgame Databases/KQKR Generator
ppc	Sigma Chess 5.1.3 Shareware.sit ƒ/Sigma Chess 5.1.3 Shareware ƒ/∑ Chess 5.1.3 ƒ/Endgame Databases/KBNK Generator
ppc	Sirtet Voyager 2.0.sit ƒ/Sirtet Voyager 2.0/Sirtet Voyager FAT'

home="$work/home"
mkdir -p "$home"

# run <arch> <path> <output file> [options...]: prints the seconds taken.
run() {
	arch=$1
	app=$2
	out=$3
	shift 3
	flag=--68k
	[ "$arch" = ppc ] && flag=--ppc
	rm -rf "$work/run-home"
	cp -R "$home" "$work/run-home"
	env HOME="$work/run-home" time -p "$bin" run "$app" "$flag" --headless --census --max "$max" "$@" > "$out" 2> "$out.err" ||
		true
	awk '/^real/ { printf "%.2f", $2 }' "$out.err"
}

# The final state: the stop line and the registers.  The low digit of the
# 68k's SR is left out: the JIT doesn't store N Z V C where the code that
# follows sets them before reading them.
state() {
	grep -E '^(instruction limit|stopped:|pc |d0 |a0 |PowerPC|r[0-9])' "$1" |
		sed 's/\(sr [0-9A-F][0-9A-F][0-9A-F]\)[0-9A-F]/\1x/' || true
}

mips() {
	echo "$1 $2" | awk '{ if ($2 > 0) printf "%.0f", $1 / $2 / 1000000; else printf "-" }'
}

printf '%-4s %-28s %9s %7s %9s %7s %9s %7s %7s %s\n' arch program interp MIPS jit-cold MIPS jit-warm MIPS speedup state
printf '%s\n' "$apps" | while IFS='	' read -r arch rel; do
	app="$games/$rel"
	name=$(basename "$rel")
	cache="$work/cache-$arch-$(printf '%s' "$name" | tr -c 'A-Za-z0-9' '_')"
	rm -rf "$cache"
	HOME="$home" "$bin" run "$app" --headless --max 1000 > /dev/null 2>&1 || true
	ti=$(run "$arch" "$app" "$work/interp.txt")
	tc=$(run "$arch" "$app" "$work/cold.txt" --jit --jit-cache "$cache")
	tw=$(run "$arch" "$app" "$work/warm.txt" --jit --jit-cache "$cache")
	n=$(sed -n 's/.*after \([0-9]*\) instructions.*/\1/p' "$work/interp.txt" | head -n 1)
	same=same
	state "$work/interp.txt" > "$work/interp.state"
	for f in cold warm; do
		state "$work/$f.txt" > "$work/$f.state"
		cmp -s "$work/interp.state" "$work/$f.state" || same="DIFFERS ($f)"
	done
	speedup=$(echo "$ti $tw" | awk '{ if ($2 > 0) printf "%.1fx", $1 / $2; else printf "-" }')
	printf '%-4s %-28.28s %8ss %7s %8ss %7s %8ss %7s %7s %s\n' "$arch" "$name" "$ti" "$(mips "${n:-0}" "$ti")" \
		"$tc" "$(mips "${n:-0}" "$tc")" "$tw" "$(mips "${n:-0}" "$tw")" "$speedup" "$same"
done
