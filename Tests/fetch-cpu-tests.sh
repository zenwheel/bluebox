#!/bin/sh
# Download the SingleStepTests 68000 vectors into .cache/sst (not committed:
# the suite has no licence).  The CPU tests skip when this has not been run.
#
# usage: Tests/fetch-cpu-tests.sh [instruction ...]   e.g. ADD.w NOP
set -eu

base=https://raw.githubusercontent.com/SingleStepTests/680x0/main/68000/v1
root=$(cd "$(dirname "$0")/.." && pwd)
dest=$root/.cache/sst
mkdir -p "$dest"

if [ "$#" -eq 0 ]; then
	# Word splitting is intended: one argument per instruction file.
	# shellcheck disable=SC2046
	set -- $(curl -fsSL https://api.github.com/repos/SingleStepTests/680x0/contents/68000/v1 |
		sed -n 's/.*"name": "\([^"]*\)\.json\.gz".*/\1/p')
fi

for name in "$@"; do
	out=$dest/$name.json
	if [ -s "$out" ]; then
		continue
	fi
	printf '%s\n' "$name"
	curl -fsSL "$base/$name.json.gz" | gunzip -c >"$out.tmp"
	mv "$out.tmp" "$out"
done
