# Oracle: real Mac OS under Basilisk II

Dev-only tooling for differential testing (PLAN.md §4). Nothing here is linked
into the product. Basilisk II and Retro68's compiler are GPL: their checkouts
and builds live **outside** this repo, in `../classic-oracle/` (next to the repo,
i.e. `~/Developer/classic-oracle/`). Apple ROMs and System software are never
copied into the repo; the oracle reads them from `~/Developer/emu/assets/`.

What is in the repo:

| Path | What |
|---|---|
| `Oracle/basilisk-trap-trace.patch` | opt-in A-line trap trace for Basilisk II (both UAE cores) |
| `Oracle/run-probe.sh` | boots System 7.1 with a probe in Startup Items, prints the probe's output |
| `Tests/Probes/` | Retro68 probe apps (`CMakeLists.txt`, `build.sh`, one folder per probe) |
| `Tests/Probes/MemRsrc/` | first probe: Memory Manager + Resource Manager |

Layout outside the repo (`~/Developer/classic-oracle/`):

| Path | What |
|---|---|
| `Retro68/` | Retro68 source (git, with submodules) |
| `Retro68-build/` | Retro68 build; toolchain in `Retro68-build/toolchain/` |
| `macemu/` | kanjitalk755/macemu checkout with the trace patch applied |
| `images/system71-base.img` | read-only, decompressed copy of `~/Developer/emu/assets/images/system71-base.img.gz` |
| `run/<Probe>/` | per-run work area: cloned disk image, prefs, XPRAM, emulator log, raw output |

## 1. Retro68 toolchain

```sh
brew install boost cmake gmp mpfr libmpc bison texinfo
mkdir -p ~/Developer/classic-oracle && cd ~/Developer/classic-oracle
git clone --depth 1 https://github.com/autc04/Retro68.git
(cd Retro68 && git submodule update --init --depth 1)
mkdir Retro68-build && cd Retro68-build
PATH="/opt/homebrew/opt/bison/bin:/opt/homebrew/opt/texinfo/bin:$PATH" \
	bash ../Retro68/build-toolchain.bash --no-carbon > build.log 2>&1
```

68k and classic PowerPC (CFM) are built. Carbon is skipped (`--no-carbon`):
there are no Apple Universal Interfaces here, so the build uses the
open-source Multiversal Interfaces, which lack Carbon. The full build takes
about 13 minutes on this machine (14 cores). Samples land in
`Retro68-build/build-target/Samples/` (68k) and `build-target-ppc/Samples/`.

## 2. Probe apps

```sh
Tests/Probes/build.sh            # RETRO68=<Retro68-build> to override
```

Builds every probe into `.cache/probes/<Probe>/` (not committed). The file to
copy to a Mac is `<Probe>.bin` (MacBinary, so the resource fork survives);
`<Probe>.dsk` is a floppy image with the app on it.

Probe conventions (see `MemRsrc/probe.c`):

- Uses only traps Classic implements or is about to (MemRsrc: Memory, Resource,
  Segment Loader, File Manager; no `InitGraf`).
- Collects one line per result in a buffer, with no addresses or other
  run-dependent values, and writes it at the end to `<Probe>.out` (TEXT/ttxt,
  CR line ends) in the folder that holds the app. The folder comes from
  `PBGetFCBInfo` on the app's own resource fork, not from the default volume.
- If a file called `AutoShutDown` is in the same folder, it calls
  `ShutDwnPower` when done. Basilisk II quits on shutdown, which ends the run.

## 3. Basilisk II with the trap trace

```sh
brew install autoconf automake sdl2 gmp mpfr pkgconf
cd ~/Developer/classic-oracle
git clone --depth 1 https://github.com/kanjitalk755/macemu.git
cd macemu && git apply ~/Developer/classic/Oracle/basilisk-trap-trace.patch
cd BasiliskII/src/Unix
NO_CONFIGURE=1 ./autogen.sh
PKG_CONFIG_PATH=/opt/homebrew/lib/pkgconfig CFLAGS="-g -O2 -std=gnu11" \
	CPPFLAGS=-I/opt/homebrew/include LDFLAGS=-L/opt/homebrew/lib \
	./configure --enable-sdl-video --enable-sdl-audio --disable-jit-compiler \
	--with-mon=no --with-bincue=no --with-vdeplug=no
make -j8
```

This is the Unix/SDL2 build (`BasiliskII/src/Unix/BasiliskII`), not the Xcode
project: the Xcode project wants SDL2.framework in `/Library/Frameworks` and
static GMP/MPFR in `/usr/local`, while this one uses Homebrew. On arm64 it
uses the `uae_cpu_2021` core with no JIT. `-std=gnu11` is needed because the
bundled slirp is K&R C, which current clang (C23 default) rejects.

### The trace

With `BASILISK_TRAP_LOG=/path/trace.log` set, every A-line opcode the 68k core
executes is written to that file (unset: nothing happens, no file). The hook
is at the top of the `0xAxxx` branch of `op_illg()` in
`uae_cpu_2021/newcpu.cpp` (and the same in the older `uae_cpu/newcpu.cpp`,
used on x86_64; that one is patched but has not been compiled here). At that
point the PC still addresses the trap word and no exception frame has been
pushed, so the registers and stack are exactly what the caller set up.

```
# seq pc trap d0 d1 a0 a1 a7 (a7)+0 +4 +8 +12
1 pc=01003e08 trap=a247 d0=0000a05d d1=00000000 a0=01003e84 a1=01085030 a7=0017fffc sp=010001d8 00000000 00000000 00000000
```

- One line per trap, numbered. `pc` and the registers are Mac addresses.
- `sp=` is the four longwords at (A7). For a Pascal-convention Toolbox trap
  called inline these are the arguments, last-pushed first, followed by the
  space for the result; OS traps take their arguments in D0/A0/A1.
- The stack is only read when A7 lies inside Mac RAM.
- Everything is logged: ROM-internal traps, the Finder, INITs, and the app.
  Filter by `pc` range (the app's code is in the application heap) or by the
  period between the app's first and last traps.
- The file is buffered (1 MB) and flushed on exit; a `kill -9` loses the tail.
  A full System 7.1 boot to the Finder is roughly 200,000 lines (~25 MB).

To rebuild the patch from a modified checkout:
`git -C ~/Developer/classic-oracle/macemu diff -- BasiliskII/src/uae_cpu_2021/newcpu.cpp BasiliskII/src/uae_cpu/newcpu.cpp > Oracle/basilisk-trap-trace.patch`

## 4. Running a probe on the oracle

One-time: make the read-only base image (the original `.gz` is never touched):

```sh
mkdir -p ~/Developer/classic-oracle/images
gzip -dc ~/Developer/emu/assets/images/system71-base.img.gz \
	> ~/Developer/classic-oracle/images/system71-base.img
chmod a-w ~/Developer/classic-oracle/images/system71-base.img
```

Then:

```sh
Tests/Probes/build.sh
Oracle/run-probe.sh .cache/probes/MemRsrc/MemRsrc.bin              # prints output
ORACLE_TRACE=/tmp/memrsrc-trace.log Oracle/run-probe.sh .cache/probes/MemRsrc/MemRsrc.bin out.txt
```

What `run-probe.sh` does:

1. Clones the base image to `run/<Probe>/disk.img` (APFS clone, so instant).
2. With hfsutils: `hcopy -m` the MacBinary probe into
   `:System Folder:Startup Items:` and puts an empty `AutoShutDown` file next
   to it. (The image is a bare HFS volume, no partition map, so `hmount`
   works on it directly.)
3. Writes its own prefs file `run/<Probe>/prefs` and starts
   `BasiliskII --config run/<Probe>/prefs`. It never reads or writes
   `~/.basilisk_ii_prefs`. Settings: Quadra 900 ROM, model ID 5, 68040 + FPU,
   16 MB, 640x480x8 window, no sound/CD-ROM/serial, JIT off, extfs limited to
   `run/<Probe>/share`.
4. System 7.1 boots, the Finder launches the probe from Startup Items, the
   probe writes `<Probe>.out` and calls `ShutDwnPower`, and Basilisk II exits.
   If it is still running after `ORACLE_TIMEOUT` seconds (default 180) it is
   killed.
5. `hcopy -r` copies `<Probe>.out` back out and the CRs become LFs.

The emulator opens a window while it runs (SDL); it does not need input.

Basilisk's extfs (a host folder as a Mac volume) is not an option on 7.1:
it needs the File System Manager (7.5 and later), and Basilisk logs
"No FSM present, disabling ExtFS". `hcopy` into a cloned image works on any
version and leaves no state behind. The prefs still point extfs at an empty
`run/<Probe>/share` so a 7.5+ image never sees the whole host disk (the Unix
default is `/`).

## Status

Working (2026-10-12):

- Retro68 68k + PPC toolchain builds with Homebrew deps; all 68k and PPC
  samples build as part of the toolchain build.
- `Tests/Probes/build.sh` builds the MemRsrc probe.
- Basilisk II (Unix/SDL2, arm64, no JIT) builds with the trace patch.
- `Oracle/run-probe.sh` is fully automatic: clone image, install probe, boot
  System 7.1, run, shut down, copy the output back. A run takes about 5 s
  of wall time. Two runs gave byte-identical output, which is committed as
  `Tests/Probes/MemRsrc/expected-system71.txt`.
- With `ORACLE_TRACE` set, that run's trace is ~420,000 traps (~54 MB) from
  boot to shutdown; the probe's own traps sit between its `_GetAppParms`
  (A9F5) and `_ShutDown` (A895, selector 1 at (A7)).

Not done yet:

- No diff tool or trace filter: comparing `expected-system71.txt` with
  Classic's run of the same probe, and cutting the trace down to the app's
  own traps, are still manual (`diff`, `grep`, `awk`).
- Only System 7.1 on a Quadra (68040). PLAN.md names 7.5.5 and 9.2.2 as the
  reference systems; those need their own images (and SheepShaver for PPC).
- The trace logs registers and four stack longwords at trap entry only: no
  results, no before/after memory, no trap names. The old `uae_cpu` core is
  patched but has not been compiled (it is only used on x86_64).
- Probes run with the Finder up, so the heap they see includes whatever the
  Finder's launch left behind; probes must keep avoiding addresses, free
  sizes and other run-dependent values in their output.
- A probe that crashes or hangs leaves the emulator up until
  `ORACLE_TIMEOUT` and then `run-probe.sh` kills it and reports no output.
- The emulator window appears on screen while a probe runs (no headless
  SDL driver was tried).
