#!/bin/sh
# Build the PowerPC probe apps (CFM) with Retro68.
#
# usage: Tests/Probes/PPC/build.sh
#   RETRO68   Retro68 build dir (default: ../classic-oracle/Retro68-build next
#             to the repo). Must contain toolchain/powerpc-apple-macos.
#
# Output: .cache/ppc-probes/<Probe>/<Probe>.bin (MacBinary).
set -eu

root=$(cd "$(dirname "$0")/../../.." && pwd)
retro=${RETRO68:-$(cd "$root/.." && pwd)/classic-oracle/Retro68-build}
toolchain=$retro/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
build=$root/.cache/ppc-probes

if [ ! -f "$toolchain" ]; then
	echo "build.sh: no Retro68 PowerPC toolchain at $retro (set RETRO68)" >&2
	exit 1
fi

cmake -S "$root/Tests/Probes/PPC" -B "$build" \
	-DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$build"

for bin in "$build"/*/*.bin; do
	case $bin in
	*.code.bin | *.rsrc.bin) ;;
	*) echo "built $bin" ;;
	esac
done
