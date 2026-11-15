#!/bin/sh
# A hibernated program resumed on another display, or at another scale:
# its screen is made the size the display now calls for, its windows kept
# on it, and it goes on.  Each program is hibernated on one screen and
# resumed (in a new process, from a copy of its folder and home) on
# smaller and larger ones; the screen it ends with must be the one asked
# for (its screenshot that size), and it must still be running, taking the
# input given after the resume.  A program with the whole screen (one
# drawing on it itself) keeps the screen it hibernated with.
#
# The sizes are the rootless Mac screens of a 1728x1117 display (a 14-inch
# MacBook Pro's) at 1x, 1.5x and 2x (1728x1100, 1152x748, 832x548, less
# the macOS menu bar, plus the classic one's 20 pixels), and of a smaller
# and a larger display at 1x.
#
# usage: sh scripts/resume-resize.sh <corpus dir> <work dir> [bluebox binary]
#   Prints one line a resume: pass or FAIL, the program, the sizes and,
#   for a failure, why; exits 1 if any fails.  Screens are kept in the
#   work dir.
#
# PLAY=1 resumes the games of scripts/hibernate-play.tsv instead, each in
# play on the default 1024x768 screen, onto SIZES (default 832x624
# 1280x960), JOBS (default 4) at a time, and prints for each resume:
# resized or kept (and why), whether it still runs, and its screenshots
# (two, 20 million instructions apart, to look at).
set -eu
LC_ALL=C
export LC_ALL
BLUEBOX_NO_TRASH=1
export BLUEBOX_NO_TRASH

one=
if [ "${1:-}" = --play-one ]; then
	one=1
	shift
fi
corpus=${1:?usage: resume-resize.sh <corpus dir> <work dir> [bluebox binary]}
work=${2:?usage: resume-resize.sh <corpus dir> <work dir> [bluebox binary]}
root=$(cd "$(dirname "$0")/.." && pwd)
bin=${3:-$root/.build/release/bluebox}
mkdir -p "$work"
work=$(cd "$work" && pwd)
failed=0

# The width and height of a PNG, from its header.
png_size() {
	od -An -tu1 -j16 -N8 "$1" | awk 'NF == 8 { printf "%dx%d\n", $3 * 256 + $4, $7 * 256 + $8 }'
}

# name, folder under the corpus, application in it, run flags, the screen
# it hibernates on, input before (it is busy at K), K, input after the
# resume, and "keeps" for a program that keeps its screen.
check() {
	name=$1 folder=$2 app=$3 flags=$4 from=$5 before=$6 k=$7 after=$8 keeps=${9:-}
	dir=$work/$name
	rm -rf "$dir"
	mkdir -p "$dir/home"
	cp -cR "$corpus/$folder" "$dir/app" 2>/dev/null || cp -R "$corpus/$folder" "$dir/app"
	# shellcheck disable=SC2086
	if ! HOME="$dir/home" "$bin" run "$dir/app/$app" --headless $flags --screen "$from" --keys "$before" \
		--max $((k + 20000000)) --hibernate-at "$k" --hibernate-to "$dir/snap.bbhib" >"$dir/hibernate.log" 2>&1 ||
		[ ! -f "$dir/snap.bbhib" ]; then
		printf 'FAIL\t%s\tdid not hibernate on %s\n' "$name" "$from"
		failed=1
		return
	fi
	for to in 1728x1100 1152x748 832x548 640x480 1600x1200; do
		[ "$to" = "$from" ] && continue
		rm -rf "$dir/h"
		cp -R "$dir/home" "$dir/h"
		png=$dir/$to.png
		rm -f "$png"
		# shellcheck disable=SC2086
		out=$(HOME="$dir/h" "$bin" resume "$dir/snap.bbhib" --headless $flags --screen "$to" \
			--keys "$after,wait:20000000,snap:$png" --max 60000000 2>&1) || true
		printf '%s\n' "$out" >"$dir/$to.log"
		want=$to
		[ -n "$keeps" ] && want=$from
		got=$( [ -f "$png" ] && png_size "$png" || echo none)
		why=
		if ! printf '%s\n' "$out" | grep -q '^resumed: '; then
			why="did not resume"
		elif printf '%s\n' "$out" | grep -qE 'halted|crashed|ExitToShell|signal'; then
			why=$(printf '%s\n' "$out" | grep -E 'halted|crashed|ExitToShell|signal' | head -n 1)
		elif [ -n "$keeps" ] && ! printf '%s\n' "$out" | grep -q 'keeps the'; then
			why="its screen changed"
		elif [ -z "$keeps" ] && ! printf '%s\n' "$out" | grep -q "^resumed on a "; then
			why=$(printf '%s\n' "$out" | grep 'keeps the' | head -n 1)
			why=${why:-its screen did not change}
		elif [ "$got" != "$want" ]; then
			# As large as its depth fits, for a screen too large for a
			# 24-bit program's video memory: as it says.
			said=$(printf '%s\n' "$out" | sed -n 's/^resumed on a \([0-9]*\)×\([0-9]*\) screen.*/\1x\2/p')
			[ "$got" = "$said" ] || why="its screen is $got, not $want"
		fi
		if [ -n "$why" ]; then
			printf 'FAIL\t%s\t%s -> %s\t%s\n' "$name" "$from" "$to" "$why"
			failed=1
		else
			printf 'pass\t%s\t%s -> %s\t%s\n' "$name" "$from" "$to" "$got"
		fi
	done
}

# One game of hibernate-play.tsv (its fields as arguments).
play_one() {
	prog=$1 k=$2 before=$3 after=$4 flags=$5
	case $prog in
	/*) path=$prog ;;
	*) path=$corpus/$prog ;;
	esac
	name=$(basename "$path" | tr -c 'A-Za-z0-9._\n-' '_')
	dir=$work/play/$name
	rm -rf "$dir"
	mkdir -p "$dir/home"
	cp -cR "$(dirname "$path")" "$dir/app" 2>/dev/null || cp -R "$(dirname "$path")" "$dir/app"
	app=$dir/app/$(basename "$path")
	runflags=$(printf '%s\n' "$flags" | tr ' ' '\n' | grep -v '=' | tr '\n' ' ')
	# shellcheck disable=SC2086
	HOME="$dir/home" timeout 900 "$bin" run "$app" --headless $runflags --keys "$before" \
		--max $((k + 50000000)) --hibernate-at "$k" --hibernate-to "$dir/snap.bbhib" >"$dir/hibernate.log" 2>&1 || true
	if [ ! -f "$dir/snap.bbhib" ]; then
		printf 'none\t%s\tdid not hibernate\n' "$name"
		return
	fi
	for to in $SIZES; do
		rm -rf "$dir/h"
		cp -R "$dir/home" "$dir/h"
		# shellcheck disable=SC2086
		out=$(HOME="$dir/h" timeout 900 "$bin" resume "$dir/snap.bbhib" --headless $runflags --screen "$to" \
			--keys "$after,wait:20000000,snap:$dir/$to-1.png,wait:20000000,snap:$dir/$to-2.png" --max 120000000 2>&1) || true
		printf '%s\n' "$out" >"$dir/$to.log"
		if printf '%s\n' "$out" | grep -q '^resumed on a '; then
			what=resized
		elif printf '%s\n' "$out" | grep -q 'keeps the'; then
			what="kept: $(printf '%s\n' "$out" | sed -n 's/.*hibernated with (\(.*\)), not.*/\1/p' | head -n 1)"
		else
			what="same size"
		fi
		end=$(printf '%s\n' "$out" | grep -E '^stopped:|^instruction limit|halted|crashed|signal' | head -n 1)
		moving=static
		[ -f "$dir/$to-1.png" ] && [ -f "$dir/$to-2.png" ] && ! cmp -s "$dir/$to-1.png" "$dir/$to-2.png" && moving=moving
		printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$to" "$what" "$moving" "${end:-no end}" "$dir/$to-2.png"
	done
}

if [ -n "$one" ]; then
	shift 3
	SIZES=${SIZES:-832x624 1280x960}
	play_one "$@"
	exit 0
fi

if [ -n "${PLAY:-}" ]; then
	SIZES=${SIZES:-832x624 1280x960}
	export SIZES
	grep -v '^#' "$root/scripts/hibernate-play.tsv" | while IFS='	' read -r prog k before after flags _; do
		printf '%s\0%s\0%s\0%s\0%s\0' "$prog" "$k" "$before" "$after" "$flags"
	done | xargs -0 -n 5 -P "${JOBS:-4}" sh "$0" --play-one "$corpus" "$work" "$bin"
	exit 0
fi

# TeachText: text typed, its window made smaller and dragged to the lower
# right (off a smaller screen), more typed after the resume.
check teachtext batch-0021/TeachText TeachText "" 1728x1100 \
	"wait:20000000,text:Hello,wait:3000000,down:1716/1088,move:1200/800,move:800/500,wait:1000000,up:800/500,wait:3000000,down:400/32,move:900/300,move:1300/600,wait:1000000,up:1300/600,wait:3000000" \
	45000000 "text: again,wait:3000000"
# Bolo: a practice game, the tank driving and firing at K, turning after.
check bolo "shareware_games_b.zip/shareware_games_b.zip/shareware_games_b/Bolo 0.99.7.sit ƒ" \
	"Bolo 0.99.7/Bolo 0.99.7" "--sound offline" 1024x768 \
	"wait:100000000,click:343/345,wait:1000000,click:642/426,wait:100000000,click:675/501,wait:250000000,keydown:q,keydown:o,keydown:space,wait:20000000" \
	490000000 "keyup:o,keyup:space,keydown:p,wait:30000000,keyup:p,keyup:q"
# Maelstrom: a game in play, which has the whole screen (it changes the
# screen's depth): it keeps the screen it hibernated with.
check maelstrom batch-0014/Maelstrom "Maelstrom/Maelstrom 1.3 ƒ/Maelstrom" "--sound offline" 1152x748 \
	"wait:100000000,text:x,wait:20000000,text:p,wait:50000000,keydown:kp5,keydown:kp4,keydown:space,wait:20000000" \
	200000000 "keyup:kp5,keyup:kp4,keydown:kp6,wait:30000000,keyup:kp6,keyup:space" keeps
exit $failed
