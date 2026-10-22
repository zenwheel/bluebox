#!/bin/sh
# M10's exit check: the game corpus runs at its original speed or better.
# Each game runs headless in a home of its own, with the interpreter and
# with the JIT, for a fixed number of instructions; it passes when it
# is still running at the end (waiting for input, or busy with an intro
# or a game) and the faster run executes at least as many
# instructions a second as the Mac it was written for (a 40 MHz 68040,
# about 35 million; a 400 MHz PowerPC G3, about 600 million).
#
# usage: sh scripts/games-m10.sh <census dir> <work dir> [bluebox binary]
#   The census dir holds corpus/.  Prints a line per game and a count;
#   exits 1 if any fails.
set -eu

dir=${1:?usage: games-m10.sh <census dir> <work dir> [bluebox binary]}
work=${2:?usage: games-m10.sh <census dir> <work dir> [bluebox binary]}
root=$(cd "$(dirname "$0")/.." && pwd)
bin=${3:-"$root/.build/release/bluebox"}
max=1500000000
mkdir -p "$work"

# One run: prints "<stop> <seconds>".
run() {
	home=$(mktemp -d "$work/home.XXXXXX")
	out=$(HOME="$home" /usr/bin/time -p "$bin" run "$1" "$2" ${3:+"$3"} --headless --census --max "$max" 2>&1) || true
	rm -rf "$home"
	secs=$(printf '%s\n' "$out" | sed -n 's/^real \([0-9.]*\)$/\1/p' | tail -n 1)
	if printf '%s\n' "$out" | grep -q 'waiting for input'; then
		stop=input
	elif printf '%s\n' "$out" | grep -q 'instruction limit reached'; then
		stop=busy
	else
		stop=$(printf '%s\n' "$out" | grep -a -m 1 -E '^stopped:|^instruction limit' | cut -c1-60 | tr ' ' '_')
	fi
	printf '%s %s\n' "${stop:-none}" "${secs:-0}"
}

# Waiting for input, or still busy when its budget ran out (an intro,
# a game animating): anything but a quit or an error.
running() { [ "$1" = input ] || [ "$1" = busy ]; }

pass=0
total=0
while IFS='|' read -r cpu target rel; do
	case $cpu in '' | '#'*) continue ;; esac
	app="$dir/corpus/$rel"
	total=$((total + 1))
	r=$(run "$app" "--$cpu" "")
	istop=${r% *} isecs=${r#* }
	r=$(run "$app" "--$cpu" "--jit")
	jstop=${r% *} jsecs=${r#* }
	imips=$(echo "$max / $isecs / 1000000" | bc)
	jmips=$(echo "$max / $jsecs / 1000000" | bc)
	best=$((imips > jmips ? imips : jmips))
	if running "$istop" && running "$jstop" && [ "$best" -ge "$target" ]; then
		result=pass
		pass=$((pass + 1))
	else
		result=FAIL
	fi
	printf '%s\t%s\t%s MIPS interpreted, %s with the JIT (wants %s)\t%s %s\t%s\n' "$result" "$cpu" \
		"$imips" "$jmips" "$target" "$istop" "$jstop" "$(basename "$rel")"
done <<'EOF'
# cpu|MIPS the original Mac ran at|path under corpus/
68k|35|Glider312.img_/Glider/Glider+ 3.1.2
68k|35|Shufflepuck1.5/Shufflepuck/Shufflepuck 1.5
68k|35|Prince_Of_Persia/Prince of Persia/Prince of Persia
68k|35|Spectre_1.1/Spectre 1.1
68k|35|bolo-0997/Bolo/Bolo 0.99.7
68k|35|crystalquest_2_2_5m.zip/Crystal Quest/Crystal Quest
68k|35|shareware_games_s.zip/shareware_games_s/Solarian II.sit ƒ/Solarian II/Solarian II
68k|35|shareware_games_s.zip/shareware_games_s/Sim Tower.sit ƒ/Sim Tower/SimTower Demo
68k|35|shareware_games_s.zip/shareware_games_s/Snood 2.0.sit ƒ/Snood 2.0 ƒ/Snood 2.0/Snood
68k|35|shareware_games_s.zip/shareware_games_s/SC-12-FAT.sit ƒ/SC-12-FAT/Star Chaos v1.2 (FAT)
ppc|600|shareware_games_s.zip/shareware_games_s/Squirt Derby.sit ƒ/Squirt Derby ƒ/Squirt Derby/Squirt Derby!
ppc|600|shareware_games_s.zip/shareware_games_s/Star Wars DroidWorks Demo….sit ƒ/Star Wars DroidWorks Demo 1.0 ƒ/Star Wars DroidWorks Demo 1.0/Star Wars DroidWorks Demo
ppc|600|shareware_games_s.zip/shareware_games_s/Sirtet Voyager 2.0.sit ƒ/Sirtet Voyager 2.0/Sirtet Voyager FAT
ppc|600|shareware_games_s.zip/shareware_games_s/StarCraft 1.05 Demo.sit ƒ/StarCraft 1.05 Demo ƒ/Starcraft 1.05/Starcraft Demo
ppc|600|TetrisMax_2.8/Tetris Max 2.8/Tetris Max 2.8
ppc|600|shareware_games_s.zip/shareware_games_s/Snood 2.0.sit ƒ/Snood 2.0 ƒ/Snood 2.0/Snood
ppc|600|shareware_games_s.zip/shareware_games_s/Spades Deluxe 2.0.sit ƒ/Spades Deluxe 2.0 ƒ/Spades Deluxe 2.0/Spades Deluxe 2.0 fc7
ppc|600|shareware_games_s.zip/shareware_games_s/SC-12-FAT.sit ƒ/SC-12-FAT/Star Chaos v1.2 (FAT)
EOF
printf '%d of %d pass\n' "$pass" "$total"
[ "$pass" -eq "$total" ]
