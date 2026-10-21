#!/bin/sh
# M9's smoke test: the Mac OS 8/9 corpus, every PowerPC application in the
# census corpus that imports one of Mac OS 8 and 9's libraries (the
# Appearance, Navigation Services, Window, Menu, Control and Dialog
# libraries, Contextual Menus, Icon Services, Open Transport, the Text
# Encoding Converter, the Sprockets).  Each one passes when it reaches its
# event loop, run headless as PowerPC, with no import missing.
#
# usage: sh scripts/smoke-m9.sh <census dir> [bluebox binary]
#   The census dir holds corpus/, ppc_list.txt and census-home/.
#   Prints one line per application and a count; exits 1 if any fails.
set -eu

dir=${1:?usage: smoke-m9.sh <census dir> [bluebox binary]}
root=$(cd "$(dirname "$0")/.." && pwd)
bin=${2:-"$root/.build/release/bluebox"}
libs='AppearanceLib|NavigationLib|WindowsLib|MenusLib|ControlsLib|DialogsLib|ContextualMenu|IconServicesLib|OTClientLib|OTInetClientLib|OpenTransportLib|OpenTptInternetLib|OTUtilityLib|InputSprocketLib|DrawSprocketLib|TextCommon|UnicodeConverter'

pass=0
fail=0
while IFS= read -r rel; do
	app="$dir/corpus/$rel"
	LC_ALL=C strings -a "$app" 2>/dev/null | grep -qxE "$libs" || continue
	out=$(HOME="$dir/census-home" timeout 300 "$bin" run "$app" --ppc --headless --census --max 60000000 2>&1) || true
	missing=$(printf '%s\n' "$out" | sed -n 's/^PowerPC imports missing: //p' | tail -n 1)
	if printf '%s\n' "$out" | grep -q 'waiting for input' && [ -z "$missing" ]; then
		pass=$((pass + 1))
		printf 'pass\t%s\n' "$rel"
	else
		fail=$((fail + 1))
		stop=$(printf '%s\n' "$out" | grep -E 'stopped:|instruction limit|ExitToShell|halted|^bluebox:' | head -n 1)
		printf 'FAIL\t%s\t%s%s\n' "$rel" "${stop:-timeout}" "${missing:+ (missing: $missing)}"
	fi
done < "$dir/ppc_list.txt"
printf '%d of %d pass\n' "$pass" "$((pass + fail))"
[ "$fail" -eq 0 ]
