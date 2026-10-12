#!/bin/sh
# Run a Retro68 probe app on the oracle (System 7.1 under Basilisk II) and
# print its output.
#
# usage: Oracle/run-probe.sh <Probe.bin> [output-file]
#
# The probe (MacBinary) is copied into the Startup Items folder of a fresh
# clone of the System 7.1 image, together with an empty "AutoShutDown" file.
# The Finder launches it at boot; the probe writes "<Probe>.out" beside
# itself and calls ShutDwnPower, which makes Basilisk II quit. The output is
# then copied back out of the image with hfsutils, CRs turned into LFs.
#
# Environment (defaults in brackets):
#   ORACLE_DIR      work area outside the repo [../classic-oracle next to the repo]
#   ORACLE_BASILISK BasiliskII binary [$ORACLE_DIR/macemu/BasiliskII/src/Unix/BasiliskII]
#   ORACLE_IMAGE    pristine System 7.1 image, never written [$ORACLE_DIR/images/system71-base.img]
#   ORACLE_ROM      Mac ROM [$HOME/Developer/emu/assets/roms/quadra900.rom]
#   ORACLE_TIMEOUT  seconds before the emulator is killed [180]
#   ORACLE_TRACE    if set, path of an A-line trap trace (BASILISK_TRAP_LOG)
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
	echo "usage: $0 <Probe.bin> [output-file]" >&2
	exit 2
fi
probe=$1
result=${2:-}

root=$(cd "$(dirname "$0")/.." && pwd)
oracle=${ORACLE_DIR:-$(cd "$root/.." && pwd)/classic-oracle}
basilisk=${ORACLE_BASILISK:-$oracle/macemu/BasiliskII/src/Unix/BasiliskII}
image=${ORACLE_IMAGE:-$oracle/images/system71-base.img}
rom=${ORACLE_ROM:-$HOME/Developer/emu/assets/roms/quadra900.rom}
timeout=${ORACLE_TIMEOUT:-180}

for f in "$probe" "$basilisk" "$image" "$rom"; do
	if [ ! -f "$f" ]; then
		echo "run-probe.sh: missing $f" >&2
		exit 1
	fi
done

name=$(basename "$probe" .bin)
run=$oracle/run/$name
disk=$run/disk.img
mkdir -p "$run/share"
rm -f "$disk" "$run/$name.out" "$run/emu.log"

# APFS clone when possible, plain copy otherwise.
cp -c "$image" "$disk" 2>/dev/null || cp "$image" "$disk"
chmod u+w "$disk"

: >"$run/AutoShutDown"
hmount "$disk" >/dev/null
hcopy -m "$probe" ":System Folder:Startup Items:"
hcopy -r "$run/AutoShutDown" ":System Folder:Startup Items:AutoShutDown"
humount >/dev/null

cat >"$run/prefs" <<PREFS
rom $rom
disk $disk
extfs $run/share
screen win/640/480/8
ramsize 16777216
modelid 5
cpu 4
fpu true
nocdrom true
nosound true
nogui true
jit false
bootdrive 0
bootdriver 0
seriala /dev/null
serialb /dev/null
ignoresegv true
idlewait true
PREFS

if [ -n "${ORACLE_TRACE:-}" ]; then
	BASILISK_TRAP_LOG=$ORACLE_TRACE
	export BASILISK_TRAP_LOG
fi
"$basilisk" --config "$run/prefs" >"$run/emu.log" 2>&1 &
pid=$!
waited=0
while kill -0 "$pid" 2>/dev/null; do
	if [ "$waited" -ge "$timeout" ]; then
		echo "run-probe.sh: emulator still running after ${timeout}s, killing it" >&2
		kill -9 "$pid" 2>/dev/null || :
		break
	fi
	sleep 1
	waited=$((waited + 1))
done
wait "$pid" 2>/dev/null || :

hmount "$disk" >/dev/null
if ! hcopy -r ":System Folder:Startup Items:$name.out" "$run/$name.out" 2>/dev/null; then
	humount >/dev/null
	echo "run-probe.sh: the probe left no $name.out (see $run/emu.log)" >&2
	exit 1
fi
humount >/dev/null

if [ -n "$result" ]; then
	tr '\r' '\n' <"$run/$name.out" >"$result"
else
	tr '\r' '\n' <"$run/$name.out"
fi
