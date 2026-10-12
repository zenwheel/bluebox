#!/bin/sh
# Build the probe apps (68k) with Retro68.
#
# usage: Tests/Probes/build.sh
#   RETRO68   Retro68 build dir (default: ../classic-oracle/Retro68-build next
#             to the repo). Must contain toolchain/m68k-apple-macos.
#
# Output: .cache/probes/<Probe>/<Probe>.bin (MacBinary, keeps the resource
# fork) plus .dsk/.APPL forms, as made by Retro68's add_application.
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
retro=${RETRO68:-$(cd "$root/.." && pwd)/classic-oracle/Retro68-build}
toolchain=$retro/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
build=$root/.cache/probes

if [ ! -f "$toolchain" ]; then
	echo "build.sh: no Retro68 toolchain at $retro (set RETRO68)" >&2
	exit 1
fi

cmake -S "$root/Tests/Probes" -B "$build" \
	-DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$build"

for bin in "$build"/*/*.bin; do
	case $bin in
	*.code.bin | *.rsrc.bin) ;;
	*) echo "built $bin" ;;
	esac
done
