#!/bin/sh
# M9's smoke test: the Mac OS 8/9 corpus, every PowerPC application in the
# census corpus that imports one of Mac OS 8 and 9's libraries (the
# Appearance, Navigation Services, Window, Menu, Control and Dialog
# libraries, Contextual Menus, Icon Services, Open Transport, the Text
# Encoding Converter, the Sprockets).  Each one passes when it reaches its
# event loop within 2 billion instructions (some intros run long), headless
# as PowerPC in a home of its own, with no import missing.  Left out: Sigma Chess's endgame database generators, which
# compute until they are done and never wait for input, and Spot Box's
# Yomavaders, a launcher for QuickTime Player.
#
# usage: sh scripts/smoke-m9.sh <census dir> <work dir> [bluebox binary]
#   The census dir holds corpus/ and ppc_list.txt; each run's home is made
#   in the work dir.  Prints one line per application and a count; exits 1
#   if any fails.
set -eu

if [ "${1:-}" = --one ]; then
	# One application: $2 the binary, $3 the work dir, $4 the path.
	home=$(mktemp -d "$3/home.XXXXXX")
	out=$(HOME="$home" timeout 1800 "$2" run "$4" --ppc --headless --census --max 2000000000 2>&1) || true
	rm -rf "$home"
	missing=$(printf '%s\n' "$out" | LC_ALL=C sed -n 's/^PowerPC imports missing: //p' | tail -n 1)
	if printf '%s\n' "$out" | grep -q 'waiting for input' && [ -z "$missing" ]; then
		printf 'pass\t%s\n' "$4"
	else
		stop=$(printf '%s\n' "$out" | grep -E 'stopped:|instruction limit|ExitToShell|halted|^bluebox:' | head -n 1)
		printf 'FAIL\t%s\t%s%s\n' "$4" "${stop:-timeout}" "${missing:+ (missing: $missing)}"
	fi
	exit 0
fi

dir=${1:?usage: smoke-m9.sh <census dir> <work dir> [bluebox binary]}
work=${2:?usage: smoke-m9.sh <census dir> <work dir> [bluebox binary]}
root=$(cd "$(dirname "$0")/.." && pwd)
bin=${3:-"$root/.build/release/bluebox"}
libs='AppearanceLib|NavigationLib|WindowsLib|MenusLib|ControlsLib|DialogsLib|ContextualMenu|IconServicesLib|OTClientLib|OTInetClientLib|OpenTransportLib|OpenTptInternetLib|OTUtilityLib|InputSprocketLib|DrawSprocketLib|TextCommon|UnicodeConverter'
mkdir -p "$work"
list="$work/m9-corpus.txt"

: > "$list"
while IFS= read -r rel; do
	case $rel in
	*"/Endgame Databases/"* | */Yomavaders) continue ;;
	esac
	app="$dir/corpus/$rel"
	LC_ALL=C strings -a "$app" 2>/dev/null | grep -qxE "$libs" || continue
	printf '%s\n' "$app" >> "$list"
done < "$dir/ppc_list.txt"

results=$(tr '\n' '\0' < "$list" | xargs -0 -P 8 -n 1 sh "$0" --one "$bin" "$work" | LC_ALL=C sort)
printf '%s\n' "$results" | LC_ALL=C sed "s|$dir/corpus/||"
total=$(printf '%s\n' "$results" | grep -c .)
fail=$(printf '%s\n' "$results" | grep -c '^FAIL' || true)
printf '%d of %d pass\n' "$((total - fail))" "$total"
[ "$fail" -eq 0 ]
