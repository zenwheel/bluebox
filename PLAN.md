# Blue Box: a Mac Toolbox environment for modern macOS and Linux

(Formerly "Classic"; renamed Blue Box on 2026-10-16. The command is `bluebox`, and its data lives in `~/Library/Application Support/Blue Box`, moved there from the old `Classic` folder automatically.)

Run unmodified classic Mac OS applications (System 7.1 – Mac OS 9.2 era, 68k and PowerPC)
as individual host applications on macOS and Linux, on top of a clean-room reimplementation
of the Macintosh Toolbox.

Long-term goal: behaviour identical to real Mac OS for every observable Toolbox behaviour,
verified by differential testing against the real system. "100% compatible" is the target, a
per-app quirks list is the reality, and the test oracle keeps us honest about the gap.

---

## 1. Decisions

| Area | Decision |
|---|---|
| Host platforms | macOS and Linux. No iPadOS. |
| Binaries | Run existing 68k and PPC binaries unmodified. No source toolchain for bluebox apps. |
| Process model | One classic app per host process, launched from the host OS. No inter-app communication except via host clipboard and files. No Finder. Revisited 2026-10-28: M21 runs Apple's own Finder, first on its own and launching programs as their own runs, then with several programs in one machine. |
| Core language | C17 for the CPU cores, memory model, trap dispatch, and the whole Toolbox. |
| Host language | Swift for the macOS host and the CLI. Python for code generation (and possibly import/install tooling). |
| Linux UI | GTK4/GDK, written in Swift (decided 2026-10-12) through a module map and a thin hand-written C shim. |
| macOS UI | AppKit, CoreText, CoreAudio. |
| Build | One SwiftPM package (C targets + Swift targets). |
| License | MIT. GPL code may be used only in the separate, never-linked test oracle. One exception, kept to its own target: `Sources/MACE` (FFmpeg's MACE decoder) is LGPL 2.1+, used only through its header; the source being public meets the LGPL's relinking terms. |
| ROM / System | Not required, never shipped. We ship our own compatibility System resource file. Anything the ROM provided is reimplemented natively or authored by us. |
| File storage | AppleDouble (`._` sidecar) is the only fork format we write. Native xattr forks are read only at import/heal time. |
| Metadata | SQLite catalog: directory IDs/CNIDs, Finder info, name mapping, resource index, font IDs, app profiles. Resource *data* stays in real resource forks. |
| Disk images | Read-only HFS (later HFS+) image support, for import and for mounting as volumes. |
| Memory | A flat 4 GB guest arena, reserved but committed lazily. No per-app memory partitions. |
| Windows | Rootless (one host window per Mac window) by default. Screen-in-a-window chosen automatically when an app needs it. |
| Speech | **MacinTalk / Speech Manager support is required.** It has three backends: Carbon `SpeechSynthesis` and `AVSpeechSynthesizer` on macOS, speech-dispatcher on Linux. |
| Extensions | Classic INITs are not loaded. Their functionality is reimplemented as built-in modules: speech, printing, QuickTime, QuickDraw GX, AppleTalk, MacTCP, Open Transport and so on. |
| 64-bit | Existing binaries stay 32-bit. New software can be written against a native 64-bit SDK built on the same Toolbox core (M11). |

---

## 2. Architecture

```
┌──────────────────────────── host process (one classic app) ────────────────────────────┐
│                                                                                          │
│  Swift host shell ── HostMac (AppKit/CoreText/CoreAudio/SpeechSynthesis/AVFoundation)    │
│        │           └ HostGTK (GTK4/GDK, FreeType, PipeWire, speech-dispatcher, cairo)    │
│        │  fills in                                                                       │
│        ▼                                                                                 │
│  struct host_ops (C ABI): windows, damage/present, events, text raster, audio,           │
│                           speech, pasteboard, file dialogs, print/PDF, profile           │
│        ▲                                                                                 │
│        │                                                                                 │
│  ┌─────┴──────────────────────── C core ─────────────────────────────────────────────┐  │
│  │  68k glue (A-line traps, Pascal/register conv.) ─┐                                 │  │
│  │  PPC glue (CFM/InterfaceLib, PPC conv.) ─────────┼─► Toolbox core (plain C fns)    │  │
│  │  Native SDK glue (opaque refs, 64-bit) ──────────┘     Memory, Resource, File,     │  │
│  │                                                        QuickDraw, Font, Window,    │  │
│  │  CPU68K interpreter    CPUPPC interpreter              Menu, Dialog, Control, TE,  │  │
│  │                                                        Event, Speech, Print, ...   │  │
│  │  Guest arena: 4 GB reserved, big-endian, accessed via generated accessors          │  │
│  │  ForkStore (AppleDouble, HFS images)   Catalog (SQLite)   Profile (Gestalt/LowMem) │  │
│  └────────────────────────────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────────────────────────┘
```

### 2.1 Guest memory

- **Arena:** at startup we `mmap(PROT_NONE)` 4 GB of address space and commit pages as they are touched. A guest address is an offset: `host = g_mem + gaddr`. The interpreter, the Toolbox and guest memory all live in the same host process.
- **Byte order:** all guest-visible data is big-endian. It is accessed only through `rd8/16/32` and `wr8/16/32` helpers, and through accessors generated for guest structs.
- **Guest struct definitions:** `WindowRecord`, `GrafPort`, `ParamBlockRec` and the rest are written once in a description file. A generator turns that into C accessors with the correct offsets and byte order. The same file also lists which low-memory globals the profile is allowed to set.
- **Memory Manager:** a real classic zone allocator inside the arena: block headers, master pointers, lock/purge/resource flags, compaction, `MoveHHi` and purging. It is ported from Executor (MIT) and checked against the oracle.
- **Growing application heap:** there is no partition size. The application zone extends upward into reserved space whenever an allocation would fail. Freed or compacted large regions go back to the host with `madvise`.
- **Reported memory:** `FreeMem`, `MaxBlock`, `PurgeSpace` and `StackSpace` report a sane cap (default 1 GB, settable per profile). This protects apps that do signed arithmetic on sizes, or that grab "all memory" for a cache.
- **`SIZE` resource:** only its flags are honoured. Apps that are not 32-bit clean run in 24-bit mode (M22: the top byte of every address ignored), with their heap and stack placed below 16 MB.
- **Host-only allocations:** Toolbox state the guest can never see (caches, scratch buffers, host objects) uses plain `malloc`.
- **Special regions:** framebuffers and host-shared buffers are placed with `mmap(MAP_FIXED)` inside the arena. Direct-write detection uses `mprotect` plus a fault handler, so normal memory accesses pay nothing.

Guest address layout (sizes illustrative):

```
0x00000000  low-memory globals, trap tables, system heap
0x00100000  application heap (grows upward)
0xE0000000  A5 world + stack (stack grows down, always above heap)
0xF0000000  fake ROM header, framebuffer(s), special regions
```

### 2.2 Toolbox layering (multi-ABI)

Each Toolbox routine is an ordinary C function. Calling-convention glue is generated, never written by hand:

- **68k:** the A-line trap dispatcher decodes OS-trap register conventions and Toolbox-trap Pascal stack conventions. Trap tables can be patched by apps (`SetTrapAddress`, `NGetTrapAddress`).
- **PPC:** the PEF loader and Code Fragment Manager. InterfaceLib, MathLib and other system libraries export native stubs. Mixed Mode (UPPs, routine descriptors) handles calls in both directions between 68k code, PPC code and native code.
- **Native SDK (M11):** the same functions behind opaque types and accessors, in host byte order, with 64-bit pointers.

### 2.3 Files

- **ForkStore** is an abstraction with three backends:
  - AppleDouble: read and write. This is the source of truth.
  - HFS/HFS+ disk images: read-only. Supports DiskCopy 4.2, raw `.dsk`/`.img`, NDIF/UDIF (including ADC compression) and hybrid ISOs.
  - xattr resource fork: read-only, used only by import/heal.
- **Writes** are atomic: write a temp file, then rename.
- **Catalog (SQLite, WAL mode):**
  - stable directory IDs and CNIDs (for aliases and `PBResolveFileIDRef`)
  - Finder info and flags, comments, icon positions
  - name mapping: MacRoman ↔ UTF-8, the 31-character limit, colons
  - a resource index (type/ID/name/size) and import provenance
  - font family IDs, app profiles, and auto-detected app modes
- **Host volumes:** chosen host directories appear as Mac volumes.
- **File locking:** File Manager deny modes map to host advisory locks. A second process opening the same file gets `opWrErr`/`permErr`.
- **Line endings:** TEXT files are kept byte-exact. CR↔LF translation is an optional per-volume setting.
- **Heal:** a startup scan finds files whose resource fork exists only as an xattr and converts them to AppleDouble. Our own tools set `COPYFILE_DISABLE=1`.

### 2.4 Display

- **One virtual framebuffer** always exists. Rootless mode is a presentation layer over it, so switching to screen mode at runtime loses nothing.
- **Rootless on macOS:**
  - Mac window order and positions are synced with the host.
  - `DragWindow` and `GrowWindow` become native move and resize.
  - Standard menus become `NSMenu`. Apps with a custom `MDEF` get a menu-bar strip drawn by us.
- **Rootless on Linux (GTK4, Wayland and X11):**
  - Each window has its own virtual Mac coordinates; host window positions are not synced.
  - Moves and resizes go through `gdk_toplevel_begin_move` and `gdk_toplevel_begin_resize`.
  - Each window gets its own menu bar.
  - Pop-up menus use `xdg_popup`.
- **Automatic screen-in-a-window:** the app is switched over when it:
  - writes directly outside its own windows (detected through `mprotect` on the framebuffer)
  - calls `HideMenuBar` or sets `MBarHeight` to 0
  - opens a window that covers the whole screen
  - calls `SetDepth`/`SetEntries` (palette changes)
  - makes video driver `Control` calls
  - uses the Display Manager or DrawSprocket

  Direct writes that stay inside the app's own window region only mark that area as damaged; the app stays rootless.
- **Scale** (done 2026-10-16: `--scale`, per app and default settings, the shell): one Mac pixel is one host point by default, which is two device pixels on a Retina display, drawn with nearest-neighbour sampling so bitmap fonts and icons stay sharp. Classic interfaces were designed for 72 dpi screens and look small on modern ones, so a scale setting (per application and as a default, in the shell, M2b) shows each Mac pixel as 1, 1.5 or 2 host points (2 = four device pixels on Retina, still whole pixels). The Mac screen size shrinks to match, the host's mouse coordinates are divided back, and window frames, menus and pop-ups scale together.
- **Remembering the mode:** the decision is stored in the app's profile, and a manual override is available.
- **Clipboard:** the Scrap Manager syncs with the host pasteboard (`TEXT` ↔ UTF-8, `PICT` ↔ PNG/PDF).
- **Standard File, Navigation Services and the print dialogs** use native host dialogs when the app installs no hooks, and a classic-style dialog when it does.

### 2.5 Fonts

- **System fonts:** the compatibility System file ships bitmap fonts with the original metrics, so layouts match, under our own names (no Apple trademarks):
  - Classic System, Classic Sans, Classic Serif, Classic Mono: generated by `Generators/fonts.py` at the classic family numbers (0, 3, 2, 4) from free look-alikes (sources and licences in `Resources/Fonts/README.md`): ChicagoFLF (public domain) for Chicago 12, ProFont (MIT, real `NFNT`s) for Monaco, and the hand-drawn OFL 1.1 Systemless bitmaps for the rest.
  - Geneva 9 is Kelsey Higham's pixel clone of the original (CC BY 3.0), with the same author's Geneva 9.1 (OFL) for the characters it lacks. Geneva 12 is uniqueidentity's bmpGeneva12 (CC BY-SA 3.0). Geneva 10, 14, 18 and 24 and New York 9 to 24 are the AccuFonts Accujen and Accuny (Doug Easterbrook, Arts Management Systems; credited by name as their licence asks).
  - Charcoal 12 (Mac OS 8/9's system font, family 2002) is Jeremy Sachs's FontStruct clone (CC BY-SA 3.0, Basic Latin, the rest synthesised).
  - Candidates for later: URW Nimbus Sans/Roman/Mono (OFL) as outline Helvetica/Times/Courier.
  - Mac OS 8's Charcoal has no redistributable look-alike; Apple built it on Chicago's metrics, so the Chicago-metric face stands in.
- **Host fonts:** a virtual "Host Fonts" resource file in the resource chain creates `FOND`, `NFNT` and `sfnt` resources lazily, only when something asks.
  - Rendering goes through CoreText or FreeType.
  - Characters map from MacRoman through each font's Unicode `cmap`.
  - Family IDs are stable, stored in the catalog, and kept clear of the classic ID numbers.
  - An allow-list keeps Font menus usable.
  - Classic names map to host equivalents: Times, Helvetica, Courier, Symbol.
- **Classic font files:** suitcases (`FFIL`) containing bitmap (`FONT`/`NFNT`) and TrueType (`sfnt`) fonts, from a host fonts folder that appears as the System Folder's Fonts folder. Classic TrueType renders through the same host rasterizer as host fonts.
- **No PostScript Type 1 / ATM:** `LWFN` outline fonts are out of scope. Documents that use them fall back to the matching bitmap `NFNT` or a host-font substitute.
- **Smoothing:** 1-bit rendering by default for exact output. Grayscale smoothing is an option.
- **Bitmap faces stay bitmaps:** Chicago 12 and Geneva 9 (and the other stand-ins) are drawn from their bitmaps at their own sizes, pixel-doubled with everything else by the display scale, never re-rendered from outlines. The user's own Apple bitmap fonts, added through the shell, replace the stand-ins for an exact look.
- **Outline fonts at the display's resolution:** text in host fonts and classic TrueType fonts is rendered at the host's real resolution (two or more device pixels per Mac pixel on Retina, times the display scale) so it looks smooth, not pixel-doubled. The framebuffer keeps the 1-bit or low-resolution version the program measures and reads back; the host draws the high-resolution glyph runs over it for display, recorded per window the way pictures already are.

### 2.6 Environment profile

Settings are resolved in layers, each overriding the one before:

1. Built-in defaults: Quadra-class identity for 68k apps, G3-class identity for PPC apps.
2. Values derived from the host:
   - `DoubleTime`, `KeyThresh`, `KeyRepThresh` and `CaretTime` from the host's input settings
   - screen size and depth
   - RAM, capped
3. Per-app profile, stored in the app bundle or `.desktop` settings and in the catalog. Auto-detected settings land here too.
4. Command-line overrides:
   ```
   bluebox run --gestalt sysv=0x0755 --lowmem DoubleTime=20 --speech=avspeech <app> [docs…]
   ```

The machine always presents ordinary Mac OS (7.x, 8 or 9 per app), never A/UX: A/UX 4.0 never shipped, and apps that detect A/UX (`'a/ux'`) tend to refuse to run or take A/UX-only paths. An `'a/ux'` override exists for experiments. A selector of our own (like the Classic environment's `'bbox'` marker) lets new software detect this environment and its host features.

Gestalt selectors can be overridden (`sysv`, `mach`, `cpu `, `fpu `, `qd  `, `sysa`, feature bits). Low-memory globals can be overridden only if they are on the whitelist of real settings. Derived state such as `ApplZone` or `CurrentA5` is never settable. The shell can push changes while an app runs, for example when the host's double-click time changes.

### 2.7 Built-in extension modules

| Module | macOS | Linux | Milestone |
|---|---|---|---|
| **Speech Manager / MacinTalk (required)** | `SpeechSynthesis` (default), `AVSpeechSynthesizer` | speech-dispatcher (`libspeechd`) | M3 |
| Printing Manager → PDF / native print | CoreGraphics PDF, `NSPrintOperation` | cairo PDF, GtkPrint/CUPS | M3 |
| Sound Manager | CoreAudio | PipeWire | M4 |
| QuickTime (classic codecs in C; newer formats via host) | AVFoundation | GStreamer | M4 |
| QuickDraw GX (graphics, typography, printing) | CoreGraphics / CoreText | cairo / Pango | M4 |
| AppleTalk (DDP, NBP, ATP, ADSP) over LocalTalk-over-UDP | BSD sockets | BSD sockets | M5 |
| MacTCP → sockets | BSD sockets | BSD sockets | M5 |
| Drag Manager ↔ host drag and drop | AppKit | GTK4 | M9 |
| Open Transport (TCP/IP and AppleTalk) | BSD sockets | BSD sockets | M9 |
| Sound input, InputSprocket, OSA, QD3D | — | — | Later |

### 2.8 Launching from the host

- `bluebox run <app> [docs…]` is the direct and development entry point.
- `bluebox install <app>` wraps a classic app as a host app.
  - **macOS:** a `.app` bundle.
    - A stub executable runs the shared runtime.
    - The classic app sits in the bundle in AppleDouble form, or the bundle references it in the library.
    - The icon is converted from `icl8`/`ICN#`/`ic**` to `.icns`.
    - Document types come from `BNDL`/`FREF`, with type/creator codes mapped to UTIs.
    - The bundle also holds the per-app profile.
  - **Linux:** a `.desktop` file, a PNG icon, shared-mime-info XML and the profile.
- Opening a document, or dropping it on the app's icon, sends `odoc`. Quitting the host app sends `quit`, so the classic app still asks about unsaved changes.

---

## 3. Repository layout

```
classic/
├─ Package.swift
├─ PLAN.md
├─ Sources/
│  ├─ CCore/            C: guest arena, logging, generated low-memory and struct definitions
│  ├─ CPU68K/           C: 68000–68040 interpreter
│  ├─ CPUPPC/           C: PPC interpreter (M8)
│  ├─ Toolbox/          C: trap dispatch and the managers (Memory, Resource, Segment Loader,
│  │                    Gestalt, ...); include/ holds the internal headers and the public machine.h
│  ├─ Modules/          C: Speech, Print, Sound, Net, ...
│  ├─ ForkStore/        C: AppleDouble, MacBinary, resource fork parsing and 'dcmp' decompression;
│  │                    later HFS/HFS+ images and xattr import
│  ├─ Catalog/          C: SQLite catalog
│  ├─ HostAPI/          C header: struct host_ops
│  ├─ HostMac/          Swift: AppKit, CoreText, CoreAudio, SpeechSynthesis, AVFoundation
│  ├─ HostGTK/          GTK4/GDK, FreeType, PipeWire, speech-dispatcher, cairo
│  │                    (Swift or plain C: decide before M12)
│  └─ classic/          Swift CLI: run, import, rsrc (later install, heal, profile, inspect)
├─ Generators/gen.py    Multiversal YAML -> struct accessors, low-memory constants, trap tables,
│                       calling-convention glue (output committed; needs Python + PyYAML only
│                       to regenerate: `uv run --with pyyaml python Generators/gen.py`)
├─ Defs/multiversal/    Toolbox interface definitions (MIT, from Executor/Retro68's Multiversal)
├─ SystemFile/          source for the compatibility System resource file + builder
├─ Tests/
│  ├─ ToolboxTests/     Swift Testing: fork store, Memory/Resource Managers, trap dispatch,
│  │                    launch, CPU vectors (run with ASan/UBSan too)
│  ├─ CPUTestSupport/   C runner for the SingleStepTests vectors
│  ├─ fetch-cpu-tests.sh  downloads the vectors into .cache/ (unlicensed, not committed)
│  ├─ Probes/           Retro68-built probe apps + expected outputs
│  ├─ Fuzz/             parsers: resource map, AppleDouble, HFS, PEF
│  └─ Corpus/           app smoke-test definitions (apps themselves not committed)
└─ Oracle/              dev-only: instrumented Basilisk II/SheepShaver fork (GPL, separate),
                        trace tooling. Requires user-supplied ROM/System, never committed.
```

---

## 4. Test strategy (applies to every milestone)

- **Probe apps:** small Retro68-built programs that exercise specific traps and write the results to a file. Each one runs on real System 7.5.5 / Mac OS 9.2.2 under the oracle emulator and in our environment, and the outputs are diffed. This runs in CI. The expected outputs are committed; the Apple software is not.
- **Trace diffing:** the instrumented oracle logs every trap call with its arguments and the before/after memory state while a real app runs. The diff tool reports the first point where our run diverges.
- **Trap census:** traces from each target app produce the exact list of traps it needs. That list is the work queue for each milestone.
- **CPU tests:** SingleStepTests 68000 JSON vectors, plus extended vectors for the 68020/030/040 addressing modes and FPU, plus PPC vectors generated from the oracle.
- **Speech tests:** a "record" backend logs every speech request (text, voice, rate, pitch, embedded commands, callback order) so behaviour can be checked deterministically in CI. Real-voice smoke tests run only on developer machines.
- **Sanitisers and fuzzing:** ASan/UBSan on all unit tests, and fuzzing of every binary parser.
- **Compatibility corpus:** an app list with scripted smoke tests (launch, open document, type, save, print to PDF, quit), tracked per milestone. A failure becomes a probe, a fix or a quirk entry.

---

## 5. Milestones

Each milestone has a goal, its scope and its exit criteria. Work inside a milestone is ordered by
the trap census.

### M0: Foundations

**Goal:** load a real classic app and run its 68k code until it hits the first unimplemented trap.

- SwiftPM package with the C core targets and the Swift CLI. Builds on macOS and in a Linux container.
- Guest arena: reserve, lazy commit, big-endian accessors.
- `Defs/` description files plus a generator for struct accessors and the low-mem whitelist.
- 68k interpreter: 68000 base plus the 68020 addressing modes TeachText needs. Validated against single-step tests.
- A-line trap dispatcher with OS and Toolbox trap tables, logging of unimplemented traps, and the `Get/SetTrapAddress` family.
- Low-memory globals initialised. Gestalt framework with the base selectors.
- AppleDouble reader. Resource Manager read side: open a resource fork, resource map in the guest heap, `GetResource` and friends, and the resource chain.
- Memory Manager: system and application zones, pointers, handles, master pointers, flags, compaction and the growing application heap.
- Segment Loader: `CODE 0` jump table, A5 world, `_LoadSeg`, `_UnloadSeg`.
- `bluebox run <app>` in headless mode with a trap trace log.
- Oracle setup: Basilisk II fork with trap tracing, the probe app build (Retro68), the diff tool, and the trap census for TeachText, SimpleText (68k), ResEdit and HyperCard 2.1.

**Exit:** TeachText's code runs through its init sequence in headless mode, and the trace shows every trap it calls; the unimplemented ones are flagged. The memory and resource probes match the oracle.

**Status (2026-10-12):** done.
- TeachText (System 7.1) launches: segments load through the jump table (including `dcmp` 0 compressed `CODE`), MPW's `%A5Init` runs, and it stops at `InitGraf` after 2,561 instructions. In census mode it reaches 4,817 instructions and lists 33 traps (10 implemented, 23 missing: the M1 work list).
- 68k interpreter passes all 1,000,060 SingleStepTests 68000 vectors (address-error cases, a documented `ASR` divergence and three bad vectors skipped).
- Decompression matches rsrcfork on all 601 compressed resources in System 7.1's TeachText, Finder and System.
- 38 tests pass on macOS and Linux (Swift 6.2 container), and under ASan/UBSan.
- The oracle works (`Oracle/README.md`): Retro68 (68k and PPC), a Basilisk II trap-trace patch, and `Oracle/run-probe.sh`, which boots System 7.1, runs a probe and returns its output in about 5 seconds. The first probe (`Tests/Probes/MemRsrc`) found two behaviours we have since matched.
- Not yet: a trace/output diff tool, 7.5.5 and 9.2.2 reference systems, 24-bit mode, `InitZone`, grow-zone procedures, a real disassembler.

### M1: TeachText on macOS

**Goal:** TeachText opens, edits, saves and prints plain text files on the host file system, in rootless windows on macOS.

- **QuickDraw:**
  - Black-and-white QuickDraw: ports, regions, patterns, all transfer modes, `CopyBits`, pictures (record and play back).
  - Color QuickDraw basics: `CGrafPort`, `PixMap`, `GDevice`.
- **Font Manager:** bitmap `FONT`/`NFNT` rendering, and the compatibility System file's Chicago, Geneva, Monaco and New York.
- **UI managers:** Window Manager with the standard `WDEF`, Menu Manager with the standard `MDEF`, Control Manager with the standard `CDEF`s, Dialog Manager, alerts, and TextEdit (unstyled).
- **Events and timing:** Event Manager (keyboard mapping through our `KCHR`, mouse, update and activate events), `TickCount` and the Time Manager basics.
- **Files:** File Manager over host volumes and the catalog: create/open/read/write/close, directory IDs, `FSSpec`s. Standard File with the native dialog when the app installs no hooks, and the classic dialog when it does.
- **Scrap and clipboard:** Scrap Manager synced with the host pasteboard.
- **macOS host (HostMac):**
  - rootless windows and window order sync
  - native move and resize
  - `NSMenu` menu bar
  - damage-tracked presentation
- **Launching:** Apple Events for `oapp`/`odoc`/`quit`, and `bluebox install` producing a macOS `.app` bundle with icon and document types.
- **Compatibility System file:** the first build of the resource file (defprocs, cursors, `KCHR`, standard strings and alerts, fonts).

**Exit:** TeachText is installed as a macOS app. Double-clicking a `.txt` file opens it in rootless windows; editing, copy/paste with host apps, Save As and quitting with the unsaved-changes prompt all work. All probes for the managers in this milestone match the oracle.

**Status (2026-10-16): complete**; what remains of its list moved to later milestones (below). **(2026-10-12):** TeachText runs rootless on macOS: each classic window is a borderless macOS window showing its slice of the hidden framebuffer through the window's shape mask, ordered as on the Mac, and the classic menu bar is the native `NSMenu` (host choices go through `MenuSelect`, after the app adjusts its menus). `--screen-window` shows the whole classic screen in one window instead; headless runs take screenshots (`--screenshot`) and scripted input (`--keys`).
- Done:
  - QuickDraw: regions, 1-bit drawing, text, `CopyBits`, `ScrollRect`
  - Font Manager, with bitmap fonts in the built System file
  - Window Manager with a native `WDEF`; Menu Manager with a native `MDEF`; Control Manager with native `CDEF`s
  - Event Manager with host input; unstyled TextEdit; Scrap Manager (in memory)
  - File Manager over host folders (AppleDouble, both forks, working directories, FSSpec calls)
  - Toolbox Utilities; placeholder Help and Printing Managers
  - screen-in-a-window host on AppKit
  - `bluebox install`: a macOS `.app` per classic app (Finder icon from `BNDL`/`FREF`/`icl8`, document types from its `FREF`s, an sh launcher); Finder and Dock opens become Open Documents events
  - high-level events: apps whose `SIZE` says so get `odoc`/`oapp` through `AcceptHighLevelEvent` in the System 7 wire format (TeachText parses it itself); Alias Manager
  - alerts as native `NSAlert`s (`ALRT`/`DITL`, `ParamText`, stages)
  - MacRoman 0x80–0xFF in the bitmap fonts: accented letters, dashes, quotes, ellipsis and ligatures composed from each face's own glyphs; the symbols (© ® ™ ° ± † § ¶ £ ¥ € ƒ, the spacing accents, ≠ ≤ ≥ ≈ ∞ √ ∑ ∏ ∂ ∆ ∫ π Ω ‰ ◊ …) drawn to each face's metrics; only the Apple logo is missing
  - native menus: opening one is a menu-bar click, so the app adjusts its menus first; `MenuSelect` waits for the host menu
  - keyboard through the host layout with dead keys (Option-E, E → é)
  - Standard File as native open and save panels (types filtered by classic file type); plain text saves stay UTF-8 with no sidecar
  - pressed close and zoom boxes
  - Sound Manager (pulled forward from M4): channels with paced command queues and callbacks at safe points, 'snd ' formats 1 and 2, standard and extended sampled sound, square-wave and wave-table notes, SysBeep; output through AVAudioEngine (MACE/IMA decoding still to do)
  - Notification Manager: messages as macOS notifications (an alert when unbundled), marks as a Dock badge, a Dock bounce, the request's sound, and response procedures at the next safe point
  - Host names longer than 31 characters get a stable short Mac name (prefix, '#', hash) that resolves back; names match ignoring case, as on HFS. The heap, stack and A5 world sit below 2 GB, since applications do signed arithmetic on addresses (free memory as ApplLimit − HeapEnd). ParamText strings appear in trap logs.
  - Headless runs keep time by instructions executed (100 ns each), so they are fast and repeatable; with a host the clock is the wall clock; `Delay` moves either forward. CopyBits converts indexed colours through a per-call table, and mask, polygon and bitmap regions are built row by row (a game that spent 53 s drawing sprites now takes under a second)
  - 68881 floating point: FMOVE in every format (packed decimal with static and dynamic k-factors), FMOVEM, control registers, the arithmetic and transcendental operations (and the 68040 rounding forms), FMOVECR constants, FBcc/FScc/FDBcc/FTRAPcc, FSAVE/FRESTORE, in doubles; Gestalt still reports no FPU, so applications that check use SANE
  - The System file has the standard cursors (CURS 1–4) and alert icons (ICON 0–2), drawn by `Generators/sysres.py`; Gestalt answers the selectors System 7.1 did for the managers we provide, and calls selector functions installed with `NewGestalt`; `GetProcessInformation` gives the SIZE flags as processMode (it said background-only); `PBHOpenDeny`/`PBHOpenRFDeny`
  - QuickTime: the Movie Toolbox reads each movie's description itself (time scale, duration, tracks, natural size), from the resource fork as classic movies kept it or a flattened data fork, and keeps time with or without decoding; libvlc, loaded at run time (VLC.app or the system's; nothing links against it), decodes into our buffer and plays the sound, and frames are drawn into the movie's GWorld when the application gives the movie time.  Movies whose description is in the resource fork play from a temporary flattened copy.  A native movie controller (play/pause, time slider, the standard actions and action filters) is a component.  Headless runs keep time without pictures or sound.  (Journeyman's Graphics- and Animation-codec movies decode.)
  - `LoadSeg` patches the calling entry too (Director projectors keep their own copy of the jump table); `NewPixPat` allocates its pixel map and data
  - `CopyDeepMask`, `SeedCFill`, `CalcCMask`, `SetPortPix`; `CouldDialog`/`FreeDialog`; mounting and ejecting are refused (one volume, always mounted); the Edition Manager starts and has nothing to publish or subscribe to
  - Fonts answer to the classic names and numbers a System 7 Mac had (Chicago, New York, Geneva, Monaco, Times, Helvetica, Courier, Symbol); the ones we have no face for draw with the nearest of ours. Sound status calls write at most the record's size.
  - The screen is 32 bits by default; `SetDepth` switches it while an application runs (1, 8 or 32 bits: the device, screenBits and every port on the screen follow, and everything is redrawn), and `HasDepth` answers for those depths. Games that want 256 colours without asking get it from a per-application setting, `bluebox set <app> depth 8`, kept in the library's Settings.json and used by every run, installed bundles included
  - CurStackBase is exactly A5 less the globals (it was rounded to 4, and MPW's runtime unpacking DATA 0 into the globals then never stopped); `--depth 1|8|32`; `Palette2CTab`/`CTab2Palette`; `DrvrInstall` accepted
  - OS traps whose HFS variant has separate glue (`PBOpenRF`/`PBHOpenRF` …) are told apart by the $0200 bit; previously the H version got every call. `InitWindows` leaves the Window Manager port current. Dialogs are colour windows, as in System 7 with Color QuickDraw. `SetScanOverride`/`GetScanOverride`. Files stored only as a sidecar exist. (ResEdit 2.1.3 and SuperPaint 2.0 start.)
  - `Fix2X`/`Frac2X`/`X2Fix`/`X2Frac`, `Microseconds`, `GetVideoDefault`; Pack7's `Str2Dec`/`CStr2Dec`/`Dec2Str`; colour fills for every shape (`FillCPoly` …, through the pattern's 1-bit form); `GetWVariant`
  - Color Picker (Pack12): `GetColor` shows a host colour panel; RGB/HSV/HSL/CMY conversions; Disk Initialization (Pack2) answers that there are no disks to initialize
  - The FCB table (`FCBSPtr`, `FSFCBLen`) and the volume's VCB (`VCBQHdr`, `DefVCBPtr`), kept current for every open file, resource files included, as applications read them directly; `PBGetFCBInfo` works on resource files (fixes a stray write of each opened file's name into guest memory)
  - Apple Event Manager: lists and records (`AECreateList`, `AEPutPtr`/`AEPutDesc`, deletion, sizes), attributes, `AECreateAppleEvent`, `AESend` to ourselves; coercion and special handlers and object accessors are recorded (nothing resolves without a scripting client)
  - QDExtensions complete: screen buffers, `GetGWorldDevice`, `QDDone`, `OffscreenVersion`
  - Sound: IMA 4:1 decoding and the uncompressed `twos`/`sowt`/`raw ` formats in compressed headers; `SndPlayDoubleBuffer` (buffers stream to the host ahead of time; doubleBack refills them); the Sound Manager also runs at interrupt time between emulation slices; selectors from older interfaces (parameter size zero) match
  - QuickDraw bottlenecks: the Std routines, and every drawing call goes through the port's grafProcs when an application installs its own; `SetStdProcs`/`SetStdCProcs`; polygons (`OpenPoly` … `FillPoly`); `BitMapToRegion`; `SetEntries` and the other device colour table calls (palette animation reaches the screen); Palette Manager odds and ends; `DeviceLoop`; fixed-point trigonometry
  - International Utilities (Pack6) with US `itl0`/`itl1` (and since M10 `itl2`, `itl4`, `itlb`, `itlc`) in the System file: date and time strings in every form, string ordering; `SecondsToDate`/`DateToSeconds`; `KeyTranslate` (US layout); `LowerText` and its variants; parameter RAM; `InitZone`; `SysError` stops with its code
  - Icon Utilities: icon suites, `PlotIconID`/`PlotIconSuite` choosing the family member for the size and depth, with masks and transforms; `PlotIconHandle`, `PlotSICNHandle`, hit testing
  - TextEdit justification (`TESetAlignment`, `TETextBox`'s argument)
  - `LaunchApplication` starts the other application as its own run (the caller quits unless it asks to continue)
  - List Manager (Pack0): the original ListRec and cell layout, custom LDEFs and a native LDEF 0, rows and columns, cell data, selection with shift/command extension and dragging, double clicks, scroll bars, `LSearch`
  - `ResourceStub` (OS trap A0FC): resource loads go through it when an application patches it, as Installer VISE does to expand its code
  - `GetVol` gives the application's folder as a working directory; menu colour tables (`GetMCEntry` and the rest); `InsertMenuItem`, `DeleteMenuItem`; `GetItemStyle` writes a word, as the ROM did
  - Headless runs stop at the instruction limit even while waiting in the Toolbox (`ModalDialog` without a filter)
  - Time Manager (`InsTime`, `InsXTime`, `PrimeTime`, `RmvTime`, drift-free extended tasks) and VBL tasks (`VInstall`, `SlotVInstall`), run as interrupts between emulation slices and at Event Manager safe points, honouring the interrupt mask
  - SANE: FP68K (Pack4) and Elems68K (Pack5) natively, in host doubles (53-bit rather than 64-bit extended precision); all formats incl. comp and 96-bit extended, decimal conversion, environment and exception flags
  - System Folder: a per-user host folder (`~/Library/Application Support/Blue Box/System Folder`, `$XDG_DATA_HOME/classic/System Folder` elsewhere) given by `FindFolder`, `SysEnvirons` and `BootDrive`; `PBHGetVolParms`, `PBOpenDF`
  - Resource Manager write side: `CreateResFile`/`HCreateResFile`/`FSpCreateResFile`, `AddResource`, `RemoveResource`, `ChangedResource`, `WriteResource`, `UpdateResFile`, `SetResInfo`/`SetResAttrs`, partial reads and writes; files are written on update, close and ExitToShell (the application and fonts keep changes in memory)
  - Color QuickDraw extras: colour tables and `Color2Index`/`Index2Color`, `NewPixMap`/`CopyPixMap`, devices, offscreen GWorlds at 1–32 bits, `GetCPixel`/`SetCPixel`, colour icons (`cicn`), colour cursors (loaded; the host pointer stays), pixel patterns through their 1-bit form, the Palette Manager (palettes kept per window, `PmForeColor`/`PmBackColor`, animation updates the entries), auxiliary control records
  - host clipboard: text in both directions at suspend and resume, as System 7 apps expect (TEToScrap/TEFromScrap)
  - Dialog Manager: dialogs from DLOG/DITL, items, edit text, ModalDialog with filters, standard filter, default and cancel items
  - Standard File extras: the app's own items (TeachText's Document / Stationery) as an accessory view in the native panel, run through the app's dialog hook in a hidden classic dialog
  - QuickDraw pictures: DrawPicture for version 1 and 2 (shapes, polygons, regions, text, patterns, all bitmap and pixmap opcodes); pictures and bitmaps print as images
  - colour: an 8-bit screen with the standard colour table; drawing, text and CopyBits at any depth
  - themes: `--appearance <scheme>` draws windows and controls from Kaleidoscope schemes (2.x: sliced sample windows from 'wnd#' layouts, widget down states, grow boxes, scroll bars, thumbs, push buttons, checkboxes and radio buttons; 1.x: scheme stripes and grow boxes on classic geometry) on a 32-bit screen; `bluebox themes` lists imported schemes
  - native style: `--appearance native` gives document windows macOS frames (close, zoom, move and resize reach the app as its own window events) and draws push buttons, checkboxes, radio buttons and scroll bars with AppKit
  - import: .zip archives, .dmg (UDIF) images, and HFS+ volumes (our own reader since 2026-10-16)
  - Component Manager (ComponentDispatch): components applications register from code or `thng` resources, instance calls, `CallComponentFunction`, capture and the rest; we register none of our own, so codec searches find nothing. Cursor Device Manager (no extra devices) and Slot Manager (no cards). `StringToFormatRec`, `ExtendedToString` and `StringToExtended` for common number formats
  - `MethodDispatch` (A9F8): the ROM's MPW Object Pascal method dispatcher, run as the same 68000 routine reading the class table and error routine from 0xBEC/0xBE8 (Skat; Photoshop 1.0 gets to its first dialog)
  - Applications' own `dcmp` decompressors run in place, the compressed data at the end of the destination buffer
  - printing: Page Setup is the host page layout panel; the printing port records each page as shapes and text runs, rendered to vector PDF (text in outline fonts, LaserWriter substitutions, runs fitted to classic widths) and printed through the host print panel; `--print-to F.pdf` for headless runs
  - pictures recorded: between `OpenPicture`/`OpenCPicture` and `ClosePicture` drawing in the port is written to the picture as version 2 opcodes (shapes with every verb, polygons, regions, lines, text, CopyBits as BitsRect/PackBitsRect/DirectBitsRect with masks, comments), each with the state it depends on; the pen is hidden meanwhile, and a hidden pen draws nothing
  - Color QuickDraw: pixel patterns in their own colours ('ppat' resources, MakeRGBPat; pictures play and record them); CopyBits' arithmetic modes and `OpColor`; colour cursors shown in colour by the host
  - depths: games known to refuse millions of colours get a depth they accept (a table by creator); an `Alert` or `ModalDialog` saying a program needs another depth ends the run, and `bluebox run` starts it again at that depth, kept as its setting
  - Gestalt reports the processor and FPU of the chosen model (a 68040 with its FPU by default, a 68020 without one)
  - floating windows (WDEF 124, procIDs 1985 to 1999, top or side title bars) and collapsing (WindowShade: two clicks in a title bar, or a scheme's collapse box), both drawn from Kaleidoscope 2 schemes' samples too
  - native style: scroll bars hit and dragged where macOS draws the knob (no arrows), pop-up menus as macOS's pop-up buttons, edit-text items in macOS's bezel
  - Standard File: file filters (old, System 7 and yourDataPtr forms) decide what the open panel offers; SFPGetFile/SFPPutFile dialog hooks in their original convention; hooks and CustomGetFile/CustomPutFile dialog filters get null events while the panel is up (an item the filter answers goes to the hook)
- Moved on: MACE decoding to M4 (since done: FFmpeg's decoder in its own LGPL target, Sources/MACE); QuickTime's music, sprite and text tracks, editing and the Image Compression Manager's own calls to M4; Mac OS 8.5 Appearance themes and the Appearance controls (sliders, tabs, themed pop-ups), scheme menus in screen mode and scheme desktop patterns to M9 with the Appearance Manager; an HFS+ reader of our own to M12. A theme for installed bundles is the application's appearance setting.
- Left: CustomGetFile's `filterProc` sees only null events (the panel is the host's, so there are no keystrokes or clicks to filter); a program that needs more would need the classic Standard File dialog of our own.

### M2: The import pipeline

**Goal:** classic software can be brought in from archives and disk images.

- **HFS images:** read-only reader for HFS images (DiskCopy 4.2, raw, NDIF/UDIF with ADC, hybrid ISO), which can also be mounted as volumes.
- **`bluebox import <file|url>`:**
  - archives (`.sit`, `.sea`, `.hqx`, `.bin`, `.cpt`) unpacked with `unar`, keeping forks
  - disk images through our HFS reader
  - everything written to AppleDouble and catalogued, with applications detected (`APPL`, 68k vs PPC)
  - works on files and URLs the user supplies; no crawler
- **`bluebox heal`:** converts xattr-only resource forks to AppleDouble.

**Exit:** a TeachText `.sit` archive downloaded from Macintosh Garden imports, installs and runs on macOS. HFS floppy images mount read-only.

**Status (2026-10-12):** the import pipeline was pulled forward and works. `bluebox import` and `bluebox run` take MacBinary, StuffIt (1.x, 5, `.sea`), Compact Pro and BinHex files (archives through `unar`). They also take DiskCopy 4.2, Disk Copy 6 NDIF (raw and ADC), raw HFS and partitioned CD images, recursing through nesting (e.g. MacBinary → StuffIt → disk image). Still to do: HFS+, MFS, UDIF `.dmg`, `.moof` flux images. (Linux parity moved to M12, after everything else.)

### M2b: The Blue Box shell (launcher, library and settings)

**Goal:** one host application is the home of the environment: it launches classic applications, brings new ones in, and holds the settings that are not per run.

- **Launcher and library:** browses the library and the classic Mac's disk, launches applications (each still its own rootless process, as `bluebox run` starts them), and shows recent and installed ones. `bluebox install` bundles remain available.
- **Import:** drop archives, disk images or folders onto the shell (or its Dock icon) to import them, with progress, the applications found, and the option to run an installer.
- **Watching the System Folder:** the shell notices what installers put into the classic Mac's System Folder and acts on it:
  - Control Panels and Apple Menu Items become launchable items (the Apple menu of every run lists Apple Menu Items, as on a real Mac);
  - Startup Items can run when the shell starts;
  - Extensions and fonts: fonts join the font list; extensions (INITs) are listed with whether the environment supports what they patch, and supported ones load at boot;
  - Preferences files are kept with the disk.
- **Settings**, in one place (the environment profile behind them stays a file `bluebox` reads):
  - per application: screen depth, CPU, appearance or theme, memory size
  - the highlight colour (HiliteRGB): by default the host's selection colour, read at launch
  - networking (M5): AppleTalk on or off, the LocalTalk-over-UDP group, the EtherTalk interface, MacTCP on or off
  - serial ports (M5b): what modem and printer ports connect to
  - fonts: add the user's own classic fonts (from their own System file or font suitcases), which then replace the open-source stand-ins
  - library location, sandboxed disk location, granted folders
- **Form:** a regular application with a window, plus an optional menu-bar status item (running classic applications, quick launch, network and port status). The status item keeps working when the window is closed, so the shell can sit in the background like the Finder did.
- **Linux (M12):** the same shell in GTK, with a tray icon where the desktop has one.

**Exit:** a user imports an installer archive by dropping it on the shell, runs the installer, and then launches the installed application and one of the control panels it added from the shell; changing a per-application setting there takes effect on its next launch.

**Progress (2026-10-16):** `bluebox shell` (Sources/bluebox/Shell.swift) has the window and the scaffolding: a sidebar of Defaults, the System Folder and every application (library and disk, with Finder icons); an application's page launches it and edits its settings; the Defaults page edits the settings every application gets (`bluebox set --default`); archives dropped on the window or Dock icon, or chosen with Import, are imported in the background; the System Folder's folders are watched and listed by kind; running and recent applications show, and a status item keeps the shell in the background. Settings so far: scale, depth, appearance (black and white, macOS frames, or a theme), highlight colour (macOS's, or a chosen one), processor. Each is one `AppSettings` field and one picker, so later options (Appearance Manager, networking, serial ports, fonts, memory size) slot in the same way.
Still to do: launching control panels and running Startup Items (needs the environment to run `cdev`s and `appe`s), loading supported extensions, Apple menu contents in every run, fonts added through the shell, library and disk locations, granted folders, the installer flow of the exit test.

### M3: MacinTalk, SimpleText (68k) and printing

**Goal:** speech is a working, required feature, and SimpleText's 68k code path runs fully, including Speak All and printing to PDF.

#### MacinTalk / Speech Manager (required)

Implement the System 7.5+ Speech Manager API:

- **Basic speech:** `SpeechManagerVersion`, `SpeakString`, `SpeechBusy`, `SpeechBusySystemWide`.
- **Channels:** `NewSpeechChannel`, `DisposeSpeechChannel`, `SpeakText`, `SpeakBuffer` (with the control flags), `StopSpeech`, `StopSpeechAt`, `PauseSpeechAt`, `ContinueSpeech`.
- **Voices:** `CountVoices`, `GetIndVoice`, `GetVoiceDescription`, `GetVoiceInfo`.
- **Parameters:** get/set rate and pitch, and `GetSpeechInfo`/`SetSpeechInfo` for the standard selectors (rate, pitch base and modulation, volume, status, callbacks, input mode, character mode, number mode, reset).
- **Phonemes:** `TextToPhonemes`, and phoneme input mode (`[[inpt PHON]]`).
- **Embedded commands:** `[[rate]]`, `[[pbas]]`, `[[pmod]]`, `[[volm]]`, `[[emph]]`, `[[slnc]]`, `[[char]]`, `[[nmbr]]`, `[[inpt]]`, `[[cmnt]]`, `[[rset]]`, `[[sync]]`.
- **Callbacks:** text-done, speech-done, sync, error, phoneme and word callbacks. These are delivered at safe points, honouring the A5 world, and through Mixed Mode in M8.
- **Gestalt:** `gestaltSpeechAttr` reports Speech Manager presence and version.

There are three backends behind `host_ops.speech`, chosen by profile or `--speech=` and with automatic fallback:

| Backend | Platform | Notes |
|---|---|---|
| `speechsynthesis` (default on macOS) | macOS | Carbon `SpeechSynthesis` (ApplicationServices) maps almost one-to-one: channels, embedded commands, phoneme mode, word and phoneme callbacks. Deprecated but present; detected at runtime. |
| `avspeech` | macOS | `AVSpeechSynthesizer`. Embedded commands are translated to SSML or utterance properties. Word callbacks come from `willSpeakRange`. Phoneme callbacks and `TextToPhonemes` are approximated. Used automatically if `SpeechSynthesis` is unavailable. |
| `speechd` (default on Linux) | Linux | `libspeechd` (LGPL), working with whatever synthesizer is installed (eSpeak NG, Piper, RHVoice) and keeping GPL synthesizers out of our binary. Embedded commands become SSML. Word and done callbacks come from index marks. Phoneme features are approximated. |
| `record` | all | Deterministic backend for tests. Logs requests and fires callbacks on a fixed schedule. |

- **Voice mapping:** classic voices (Fred, Ralph, Kathy, Junior, Victoria, Zarvox, Whisper, Bad News, Good News, Trinoids, Cellos, …) map to the same-named macOS voices where installed. Otherwise, and on Linux, they map by gender and age to the closest host voice, with the mapping in a profile table. Voice IDs (`VoiceSpec` creator/id) are stable across launches and backends.
- **Speech exit criteria:**
  - Every Speech Manager probe matches the oracle on API results and the order of callbacks, using the record backend.
  - SimpleText's Speak All and the classic voices work on macOS with both macOS backends, and on Linux with speech-dispatcher plus eSpeak NG.
  - A probe that switches voices, rates and embedded commands mid-utterance produces the expected sequence of callbacks.

**Status (2026-10-16):** the Speech Manager is in (Sources/Toolbox/speech.c): every call above but `SpeechBusySystemWide` beyond this process and real dictionaries; channels speak through NSSpeechSynthesizer on macOS (Sources/BlueBoxHost/Speech.swift), which takes the classic embedded commands and has most MacinTalk voices by name (Agnes and Victoria fall back to the default unless downloaded); words, sync, phoneme and done callbacks are delivered at safe points; headless runs finish utterances at once. SimpleText's Speak All speaks. Since (2026-10-18): `--speech avspeech` (AVSpeechSynthesizer, also the fallback when NSSpeechSynthesizer is missing; embedded commands split the utterance into utterances, words come from `willSpeakRange`) and `--speech record:FILE` (requests logged, words, syncs and done delivered at once; SimpleText's Speak All reaches it). Still to do: `speechd` on Linux (M12), and checking callback order against the oracle with the record backend. Since (2026-10-22): every voice speaks with the host's system voice (Spoken Content's, else the language's default; Siri voices aren't open to other programs), the MacinTalk Pro voices (Agnes, Bruce and Victoria, high quality) are listed, and any VoiceSpec opens a channel, so Mouth finds "Victoria, high quality"; macOS no longer reports phonemes from either synthesizer, so phoneme callbacks are made up from each spoken word by letter-to-sound rules and paced over the word (with the record backend, all at once), ending with a silence.

#### SimpleText and printing

- **Styled TextEdit:** `TEStyleNew`, `styl`/`TEXT` documents and `soup`, plus Drag Manager basics for dragging text within a window.
- **Printing Manager:**
  - `PrOpen` through `PrClose`, `TPrint`/`PrValidate`, and `PrGeneral` resolution queries
  - a printer `GrafPort` that records QuickDraw; each page plays back into PDF (CoreGraphics on macOS, cairo on Linux)
  - real PDF text with embedded fonts, vector shapes, and full-resolution images
  - native Page Setup and Print dialogs when the app installs no hooks, and the classic dialog with "Save as PDF…" and "Print…" when it does
  - the environment identifies as a LaserWriter 8 driver
  - PostScript `PicComment`s: the QuickDraw fallback is used
- **Quick start mode:** a printing profile option to always save to PDF in a chosen folder.

**Exit:** SimpleText (68k) opens, edits and saves styled documents, speaks them with selectable voices on both platforms, and prints a selectable-text PDF or prints to a host printer.

**Status (2026-10-18):** on macOS the exit is met. Styled TextEdit (Sources/Toolbox/te.c) keeps Apple's TEStyleRec, style table, line heights, null style and style scraps, and draws, measures and wraps run by run; SimpleText shows, saves (`TEXT` + `styl`) and reopens styled documents, and copying puts `TEXT` and `styl` on the scrap. Written forks reach the disk at each event call, so a document saved and kept open is not lost. The Drag Manager (drag.c) covers drags within the program's windows (TrackDrag with tracking and receive handlers, flavors, promised data, hilites, WaitMouseMoved), and with TEGetHiliteRgn SimpleText moves text by dragging. Printing: real PDF text at the right size; PrGeneral (72-600 dpi, scaled back to points in the PDF; draft bits; orientation); the classic job and style dialogs for programs that extend them (PrJobInit/PrStlInit/PrDlgMain, with Save as PDF and Page Setup buttons); the print-folder setting; picture comments recorded in pictures and given to programs' commentProcs when drawn, with TextBegin/TextCenter rotation in the PDF and the QuickDraw fallback of PostScript comments (2026-10-22). Not done: drags to and from the Finder and host windows (`hfs ` flavors, clippings), `soup`, and Linux (cairo, speechd: M12). Notes: QuickDraw may not scale bitmap fonts to sizes without a strike (seen at 24 pt; to check); the census misses about 15 programs that are inside disk images in the corpus; the Fetch image only looked damaged (a Disk Copy 6 image whose first chunk is stored as it is; fixed 2026-10-19).

### M4: QuickTime and QuickDraw GX

**Goal:** QuickTime 2.x-era movies play with sound, and QuickDraw GX applications draw and print.

- **Sound Manager** (needed by QuickTime, so it moves here): `snd ` playback (formats 1 and 2, sampled sound, the basic synthesizers), `SndPlay`, channels, `SndDoCommand`/`SndDoImmediate`, callbacks, and MACE and IMA4 decompression. Output through CoreAudio and PipeWire.
- **Component Manager:** registration, `FindNextComponent`, `OpenComponent`, selector dispatch through `CallComponent`, and `thng` resources, so QuickTime's codecs and controllers can be components (ours, or third-party 68k ones).
- **QuickTime (Movie Toolbox):**
  - `EnterMovies`, movie files and `moov` atoms, `NewMovieFromFile`, `StartMovie`/`MoviesTask`, time bases, tracks and media, and the standard movie controller
  - the Image Compression Manager, including compressed `PICT`s (`DecompressImage`, JPEG in `PICT`)
  - **codecs:** our own C decoders for the bluebox set (Raw, Apple Video `rpza`, Animation `rle `, Graphics `smc `, Cinepak, Photo-JPEG). Host frameworks have dropped several of these (macOS removed Cinepak), so we can't rely on them for old codecs; AVFoundation or GStreamer handle newer ones.
  - frames are decoded into GWorlds and drawn through `CopyBits`, so rootless windows and screen mode both work
  - from M1: music tracks (the QuickTime Music Architecture's notes, which VLC cannot render), sprite and text tracks, editing, and the Image Compression Manager's own calls
- **QuickDraw GX:**
  - graphics: shapes, styles, inks, transforms, the view port and view device model, and drawing through CoreGraphics or cairo
  - typography: GX fonts and layout through the host text stack
  - GX printing (the 7.5–7.6 printing architecture) mapped onto the same PDF and host-print path as the Printing Manager
  - scope: the subset the GX applications in the corpus actually use (chosen at the start of M4); GX is large and few apps used all of it
- **Gestalt:** the QuickTime and GX selectors report what is implemented.

**Exit:** the 68k QuickTime 2.x Movie Player plays a Cinepak movie with sound, in rootless mode and in screen mode; HyperCard-free test apps play `snd` resources; a GX sample application draws shapes and styled text on screen and prints them to PDF.

**Status (2026-10-19):** the list is done; checks on a real Mac remain for the new parts.
- Codecs of our own (Sources/Toolbox/qt_codec*.c): Cinepak, Animation, Graphics, Apple Video, None, Photo-JPEG (host decoder); sound from raw, `twos`/`sowt`, IMA 4:1, MACE. They cover nearly every movie track in the corpus. Movies play through qt_native.c (sample tables, edit lists, key frames, sound queued a second ahead), headless runs included; VLC is the fallback (Sorenson, QDesign; compressed `cmov` headers are read for it).
- Music tracks (qt_music.c) and the Tune Player and Note Allocator components (qt_tune.c) play through the host's General MIDI synthesizer; checked by ear against a real Mac (Snow's Silent Night); Sword Dream 3D's soundtrack plays through the tune player.
- Text and sprite tracks (qt_text.c, qt_sprite.c) composite with the video: styled text through our TextEdit, sprites with matrices, layers, image changes and graphics modes. scripts/make-test-movie.py writes test movies; checked on a real Mac with QuickTime 2.5 and in Blue Box: Captions.mov and Sprites (key frames).mov play the same on both. That QuickTime plays sprite tracks only as whole key frames: Sprites.mov's override samples (a key frame, then only the properties that change) leave its sprites in the first frame alone, while Blue Box applies them as later QuickTimes did. Left: scrolling and karaoke text, wired sprites (QuickTime 3).
- Editing (qt_edit.c): cut, copy, paste, clear, insert and delete of movie segments by edit lists, selections, the movie scrap, undo, saving the 'moov' resource, the controller's editing calls. Pasting media from another file isn't done (only within one movie's file); FlattenMovieData and new movies made from scratch (NewMovieTrack, AddMediaSample) aren't either. SimpleText's controller doesn't enable editing, so the corpus hasn't exercised it.
- The Image Compression Manager: decompression (images, sequences, `$8200` pictures) and compression (None, Animation, Photo-JPEG; images, sequences, pictures; the Standard Image Compression component without its dialog). PowerPC programs can't reach the `scdi` calls yet (no SC* exports in ppc_libs.c).
- QuickDraw GX (gx.c, defs in Generators/defs/gx.yaml): the printing architecture and the shapes it uses (rectangles, pictures, converted PICTs, mappings, window view ports, QuickDraw translators) printed through the Printing Manager to PDF, and the Collection Manager; Gestalt reports GX 1.1 ('qdgx', 'pmgr'). ShareDraw and SimpleText print through GX. GX's own drawing (paths, curves, typography, inks and transforms beyond scale and translation) isn't drawn: no program in the corpus draws with it.
- Fixed along the way: pictures keep their RGB colours in colour ports; UniqueID picks a random unused ID (Sword Dream 3D looped for ever on 128); files copied by Finder keep their types and resource forks (macOS's own attributes).
- Not checked: playback in screen mode (`--screen`), Movie Player itself (not in the corpus outside the QuickTime installers).

### M5: Classic networking (AppleTalk and MacTCP)

**Goal:** AppleTalk network games such as Spectre find and play each other, and MacTCP applications reach the internet.

**Status (2026-10-18), AppleTalk:** the `.MPP` driver is in (Sources/Toolbox/appletalk.c) on LocalTalk over UDP (multicast 239.192.76.84:1954 with a 4-byte sender ID; `--appletalk local`, the default, keeps it on the loopback interface so only copies on this host meet; `--appletalk lan` joins the local network on the interface the system picks, `lan:en0` on a chosen one, also offered in the shell's settings; `off`). LLAP node acquisition (ENQ/ACK), DDP sockets with 68k socket listeners called as the driver called them (RHA at `toRHA`, `ReadPacket`/`ReadRest`, our node and network in `ABusVars` where programs read them), `WriteDDP` with short headers on network 0, self-send, and NBP register (with verify), lookup (wildcards, our own names included), confirm, remove and kill; Gestalt `atlk` 58. Two copies of Bolo 0.99.7 find each other through NBP and play over DDP (with `--realtime`, below). Spectre 1.1's Net Play registers its name and waits; what it does next is still to see. Since then: ATP (the `.ATP` driver: requests with retries and exactly-once release, GetRequest, SendResponse with kept responses, AddResponse, the kills), ADSP (the `.DSP` driver: connection ends and listeners, the open handshake in all four modes, sequenced data with acknowledgement, retransmission and EOM, attention, close and forward reset, user routines), AEP's echoer, ZIP's calls through `.XPP` as a network without a router answers them, the LAP Manager and its AppleTalk Transition Queue, InterfaceLib's AppleTalk calls for PowerPC applications, and EtherTalk: `--appletalk bridge:en0` (and the shell's setting) runs AppleTalk Phase 2 on a wired interface (SNAP framing, AARP probing, requests and gleaning, long DDP headers, the network number from RTMP or the startup range), the BPF device opened by `bluebox bpf-helper` run as root through the administrator password prompt, which passes the descriptor back and exits. Spectre's Net Play works as far as its copy protection: a second copy with the same serial number is found through NBP and refused ("in use by"), as on Macs. Since (2026-10-19): ASP and an AFP 2.x client (afp.c, asp.c; also AFP over TCP through DSI), and AppleShare volumes mirrored into a cache folder mounted for the program (appleshare.c: `--appleshare tcp:host/Volume` or `atalk:Server:AFPServer@*/Volume`, and a setting): folders and placeholder files made when the session opens, forks fetched on first open, changes sent back as they happen. Checked against netatalk 4.5 over TCP, and over AppleTalk through the EtherTalk bridge from a wired Mac (the user's); over Wi-Fi the server's NBP name wasn't found, and bridging onto Wi-Fi is refused (2026-10-19). macOS hands programs without an entitlement 02:00:00:00:00:00 for every interface's address; frames now go out from the real one (IOKit, or a virtual interface's EUI-64 link-local IPv6 address), which UTM's bridge100 needs, but Wi-Fi still heard nothing from the server with it. A failed lookup says how many AppleTalk frames were heard on the interface. Still to do: routing (zones other than "*", NBP through a router), testing the bridge with real hardware and another emulator, and two Spectres with different serial numbers. Headless runs take `--realtime` so that two copies' clocks agree; without it a headless run's time follows its instruction count and network peers time each other out.

**Mouse modes (2026-10-18):** programs that read the mouse by recentring the cursor (RawMouse/MTemp and CrsrNew: Ingemar's skiing game) or through an ADB service routine (Apeiron) work: the program's cursor moves are honoured, ADB packets carry the mouse's own motion, and the host captures the mouse while a program recentres it. Prince of Persia in colour no longer hangs for the user.

**Status (2026-10-18), MacTCP:** MacTCP is in (Sources/Toolbox/mactcp.c, on a small set of native Device Manager drivers in drivers.c). `.IPP` opens by name, TCP streams (create, active and passive open, send, receive, no-copy receive, close, abort, status, release) and UDP streams (create, read, write, MTU, release) run on host sockets, asynchronous calls complete through their ioCompletion routines, and notification routines hear of data arriving and of the other end closing. The resolver is a `MacTCP` control panel (`cdev`/`ztcp`) written into the System Folder at launch, whose `dnrp` jumps to native code: `StrToAddr` (dotted addresses answer with no name, as programs' `inet_addr` expects), `AddrToStr` and `AddrToName`, answered through the result procedure. NCSA Mosaic 1.0.3 loads and renders pages from a local web server, and two copies of Bolo 0.99.7 play a UDP/IP game (joined by the host's LAN address: Bolo checks the address the host reports against the one it dialled, so 127.0.0.1 doesn't do). Fetch 2.1.2 lists and transfers with real FTP servers in active mode (2026-10-19): its address is known from the start of a connection, and a PORT command's port listens at once, since Fetch opens the listener only after the server's reply and servers such as pyftpdlib connect on PORT. Still to do: an IRC client, and MacTCP's Gestalt (`mtcp`) and low-memory presence.

- **AppleTalk, implemented natively** (not Apple's drivers): DDP sockets, NBP (register, lookup, confirm), ATP, ADSP, ASP basics, AEP (echo), and the `.MPP`, `.ATP`, `.XPP` and `.DSP` driver interfaces through the Device Manager, so both the high-level AppleTalk Manager calls and raw `PBControl` calls work.
- **AppleTalk Transition Queue** and the Chooser-equivalent settings (user name, zone) in the environment profile.
- **Link layer: LocalTalk over UDP ("LToUDP")**, LocalTalk frames on a UDP multicast group. Other emulators and AppleTalk routers use the same convention (to confirm when this milestone starts), so instances of this environment can play each other, and emulators and real Macs (through a router) on the same network.
  - LLAP node address acquisition, broadcast, and the multicast group configurable in the profile
  - a host-only loopback mode, so two apps on one machine can play without a network
- **Link layer: EtherTalk (optional), bridged onto a wired Ethernet**, for AppleTalk with real Macs, netatalk file servers and routers on the local network that do not speak LToUDP. Bridging is only for these raw AppleTalk/EtherTalk frames, never for TCP/IP, and only over a wired interface (Wi-Fi access points do not pass frames from a second MAC address). It needs raw Ethernet frames, so it is optional and set up once from the shell (M2b), which offers only wired interfaces:
  - macOS: `vmnet.framework` in bridged mode (needs the networking entitlement, or a small privileged helper), or BPF through a helper that holds the device
  - Linux: a `tap` device or an `AF_PACKET` socket, granted `CAP_NET_RAW` to a helper
  - AARP for node addresses, the AppleTalk Phase 2 multicast addresses, and zone information from a seed router when there is one
  - Without it, AppleTalk still works between instances and LToUDP emulators, and a router (e.g. jrouter or a real AppleTalk router) can join LToUDP to EtherTalk.
- **MacTCP** on host sockets: the `.IPP` driver interface (TCP streams, UDP), and the DNR resolver (`StrToAddr`, `AddrToName`) through the host resolver. TCP/IP is never bridged: MacTCP here and Open Transport (M9) always use the host's own sockets, so they work over any interface (Wi-Fi included), with no privileges, and through VPNs.
- **Gestalt and low memory** report AppleTalk and MacTCP as present.

**Exit:** two copies of Spectre (and a second AppleTalk game, e.g. Bolo) on two hosts find each other through NBP and play a networked game; one of them also interoperates with another emulator on the same LToUDP network; a MacTCP client (Fetch or an IRC client) connects to a real server.

### M5b: Serial ports

**Goal:** applications that use the modem and printer ports (terminal programs, file transfer, MIDI and serial devices, LocalTalk-port printers) work, connected to real hardware or to the host.

**Status (2026-10-18):** the Serial Driver is in (Sources/Toolbox/serial.c): `.AIn`/`.AOut`/`.BIn`/`.BOut` as native drivers, with reads (synchronous, or asynchronous and completed when all their bytes have come), writes, `SerGetBuf`, `SerStatus`, `SerReset` (baud rate, data bits, parity and stop bits passed to host devices) and the other control codes accepted; the PowerPC Serial Manager calls go to it. `--modem-port` and `--printer-port` connect a port to a Hayes modem (`modem`, the modem port's default: `ATDT host:port` dials TCP, Telnet negotiation on port 23 or when the other end starts it, `+++`, `ATH`, `ATO`, `ATE`/`ATV`/`ATQ`, S-registers; `modem:PORT` also rings and answers on PORT), `tcp:HOST:PORT`, `listen:PORT`, `pty`, a host device, or `none`. The Communications Resource Manager's `CRMSearch` lists both ports (PowerPC, and `_CommToolboxDispatch` for 68k with selectors still to check against the oracle). ZTerm 1.1b7 dials a local test server through the modem and talks through a pty. Still to do: the shell's port settings (M2b), a real USB serial adapter, hardware handshake, `PortAUse`/`PortBUse`/`SPConfig`, the rest of the Communications Toolbox, and MIDI.

**Status (2026-10-20), the Communications Toolbox:** `_CommToolboxDispatch` and InterfaceLib's exports of it (ctb.c) carry the whole of the Connection, Terminal and File Transfer Managers, the utilities (`AppendDITL`, `CountDITL`, `ShortenDITL` on the Dialog Manager's; `StandardNBP` cancels) and the Communications Resource Manager (a device queue with both ports, `CRMGetIndToolName`, the resource calls), with tools of our own: the Serial Tool and Apple Modem Tool on serial.c's ports (the modem tool dials `PhoneNumber` and answers rings) and a TCP Tool (Host, Port, Telnet, LocalPort) for connections (ctb_cm.c: sync and async reads and writes with completion routines, `CMStatus`, searches, listen and accept, break, the parameter block calls); TTY and VT102 tools for terminals (ctb_tm.c: cursor addressing, erasing, insert and delete, scrolling regions, bold, underline and inverse, the line drawing set, reports, keys and the keypad, selection, the cacheProc, searches; drawn in Monaco); Text and XMODEM (checksum, CRC and 1K) for file transfers (ctb_ft.c, with `FTSend` and `FTReceive`). Configurations are strings (`Baud 9600 Parity None Port "Modem Port"`); the Choose calls show a dialog of the tools with the configuration string to edit; the Setup calls add nothing to a program's own dialog. Gestalt `ctbv`, `conn`, `term`, `fxfr`, `crm ` and `ditl`. Still to do: trying programs built on it, and Setup items for the tools.

- **The Serial Driver:** `.AIn`/`.AOut` (modem port) and `.BIn`/`.BOut` (printer port) through the Device Manager: `SerReset`, `SerSetBuf`, `SerHShake`, `SerSetBrk`/`SerClrBrk`, `SerGetBuf`, `SerStatus`, and the `PBControl`/`PBStatus` codes behind them; reads and writes asynchronous with completion routines.
- **What each port connects to** (chosen in the shell, M2b, or the profile):
  - a host serial device (`/dev/cu.*` on macOS, `/dev/ttyS*`/`/dev/ttyUSB*` on Linux), with baud rate, data bits, parity, stop bits and handshake (XON/XOFF, hardware) passed through
  - a pseudo-terminal the host can open (`screen`, `minicom`, another emulator)
  - a named pipe or a Unix socket
  - a TCP connection (raw, or Telnet with RFC 2217 serial control) to a host and port, or listening for one
  - a Hayes modem emulation: `ATDT host:port` dials a TCP connection, `ATA` answers one, `+++` and `ATH` hang up, so BBS and terminal programs work unchanged
  - nothing (the port is present but unplugged)
- **Port sharing:** the printer port is shared with LocalTalk; when AppleTalk uses LocalTalk over UDP the printer port stays free for serial use, as the environment has no physical LocalTalk.
- **The Communications Toolbox** (Connection, Terminal and File Transfer Managers with the Serial tool) for applications built on it, after the driver. (Done: see the status above.)
- **MIDI:** the MIDI Manager's serial port drivers on top of the same ports, or CoreMIDI/ALSA directly.
- **Low memory and Gestalt:** `PortAUse`, `PortBUse` and `SPConfig` say which ports are in use and by what; the serial Gestalt selectors report both ports.

**Exit:** a terminal program (e.g. ZTerm) talks to a USB serial adapter and, through the modem emulation, to a Telnet BBS; a second instance connects to the first through a pty.

### M6: ResEdit and screen-mode detection

**Goal:** ResEdit 2.1.3 runs well enough to inspect and edit real applications. This stress-tests the Resource Manager.

- **Resource Manager write path:** `AddResource`, `ChangedResource`, `RmveResource`, `UpdateResFile`, `WriteResource`, `SetResAttrs`, `SetResInfo`, map compaction, and creating resource forks on files that lack them. Always written atomically to AppleDouble.
- **More managers:** List Manager with custom `LDEF`s, custom `MDEF`/`CDEF` support, pop-up menus (`CDEF 63`), Color Picker and offscreen GWorlds.
- **Color QuickDraw:** complete enough for ResEdit's icon, `cicn`, `ppat` and `clut` editors.
- **Raw fork access:** `OpenRF` with `FSRead` on raw fork bytes, which must be byte-exact.
- **Automatic screen-in-a-window detection** (§2.4): framebuffer `mprotect` and the fault handler, the trigger heuristics, the runtime switch, and saving the decision to the profile.
- **Profile system complete:** the layered profile, CLI overrides, the Gestalt and low-mem whitelist, and values derived from the host.
- **`bluebox inspect`:** a host-side resource browser with export (`PICT`/icons → PNG, `snd` → WAV, `STR#`/`TEXT` → UTF-8). (Done 2026-10-20: a summary for each resource's contents; PICT drawn by our QuickDraw and icons, including `cicn`, to PNG; sampled `snd` (standard, extended, MACE and IMA 4:1) to WAV; `STR`, `STR#`, `TEXT` and `vers` to UTF-8; other resources raw when chosen with `--type`/`--id`.)

**Exit:** ResEdit opens a copy of TeachText, edits a `STR#`, a `DLOG` and an icon, saves, and the modified TeachText runs. Fuzzed resource maps never crash the runtime. A test app that writes directly to the screen switches to screen mode on its own.

**Status (2026-10-20): done.** ResEdit 2.1.3 opens a copy of TeachText, edits a `STR#`, an `ICON`, a `cicn` and a `DLOG` (the bounds), saves, and the copy runs. That took: dialogs ResEdit builds itself from a window (kind 135) taking DialogSelect's keys; ReleaseResource/DetachResource keeping a changed resource (resAttrErr); StillDown false while a mouse event waits (double clicks); the System's `PACK` resources and programs' own packages (ResEdit's `PACK` 1, BitEdit) run through their traps. Twenty thousand damaged forks go through the parser and the Resource Manager without a crash (also under AddressSanitizer). Screen mode by itself: a program that writes to the screen outside its windows (the CPUs watch the framebuffer's memory), hides the menu bar, changes depth or reserves a DrawSprocket context gets the whole screen, and that becomes its `display` setting. The profile: the host's double-click and key-repeat times, `--lowmem` for the real settings, Gestalt `blbx`.

### M7: HyperCard 2.1

**Goal:** HyperCard 2.1 runs the Home stack and typical stacks, including sound, XCMDs and printing.

- **Sound:** the Sound Manager from M4, exercised by HyperCard's `play` and stack sounds.
- **XCMD/XFCN:** the callbacks themselves are HyperCard's own code, but loading the code resources, the A5 world and the calling conventions are ours to get right.
- **Heavier QuickDraw:** all `CopyBits` modes with masks, `CopyMask`/`CopyDeepMask`, `SeedFill`/`CalcMask`, and pattern and region performance.
- **Fonts:**
  - host fonts as system fonts (the virtual resource file, stable IDs, allow-list)
  - classic font suitcases (bitmap and TrueType) from the host fonts folder
- **Other managers:** Script Manager and International Utilities basics, the Help Manager (balloons), and Notification Manager basics.
- **Printing at scale:** printing cards, reports and scripts.
- **Performance:** an interpreter profiling pass and pre-decoded instruction caches.

**Exit:** HyperCard 2.1 runs its Home stack and a corpus of ten-plus stacks from Macintosh Garden: navigation, painting, sound, an XCMD-based stack, printing cards to PDF, and speech through a stack XCMD or HyperTalk `speak` where available.

**Status (2026-10-20): done.** HyperCard 2.1 runs its Home stack; its own XCMDs (Palette, Script Editor, Picture, the watchers) and the Home stack's `FontExists` XFCN run, and Palatino (family 16, drawn with our serif face) no longer draws a missing-font warning. Stacks are found when "HyperCard Stacks" sits in the HyperCard folder, as the installer leaves it. Headless runs need two clicks before HyperCard shows its card window (it waits on OSEventAvail for an OS event, and the first click only activates); a person's first click does that.

### M8: PowerPC

**Goal:** PowerPC-native applications run, and fat binaries prefer their PPC code.

- **PPC interpreter:** 32-bit user mode (603/604/G3 integer and FPU), validated against vectors generated by the oracle.
- **PEF loader and Code Fragment Manager:** `cfrg`, data fork fragments, imports and exports, init/term routines, weak imports.
- **System libraries:** InterfaceLib, MathLib, ObjectSupportLib and the PPC side of every Toolbox call implemented so far.
- **Mixed Mode:** `NewRoutineDescriptor`, `CallUniversalProc`, and switching between 68k, PPC and native code in all directions, including callbacks and custom defprocs written in either ISA.
- **Speech callbacks** through Mixed Mode for PPC apps.

**Exit:** SimpleText PPC and HyperCard 2.4 (PPC) reach parity with their 68k runs from M3 and M7. A fat app picks PPC by default; the profile can force 68k.

**Status (2026-10-20): done.** HyperCard 2.4.1, PowerPC and 68k, runs the Home stack, moves between cards (Go menu keys, the card buttons, the message box), opens the Practice and Addresses stacks and shows the Tools palette and the Navigator, with the same screen pixel for pixel either way. That took: generated InterfaceLib glue going through the trap table for a Toolbox trap the program has patched (HyperCard patches SelectWindow and ShowWindow); WaitNextEvent taking its events through a program's GetNextEvent patch; an activate event whenever the front window becomes the active one; a 'KCHR' 0 in the System file (HyperCard wants one before it takes Return in the message box); SaveOld and DrawNew repainting what a window leaves when the program moves it itself. SimpleText was never PowerPC code: every copy in the corpus and on the Mac OS 8 and 9 disks (1.3, 1.4) is 68k only, so it runs as at M3 whatever the setting. A fat application runs its PowerPC code unless its cpu setting is 68020 or 68040 (or `--68k`). Internet Explorer 5.1 now runs as PowerPC code (fragments' exports are known before the libraries they import, which a cycle between its C++ and memory libraries needs; MathLib's float classification calls; OSDispatch $40 and $41), so no fat application in the census works only as 68k. Census (2026-10-20, the 68k one run with `--68k`): 178 of 225 PowerPC runs and 790 of 1,036 68k runs reach their event loop; of the 158 fat applications, 126 do as PowerPC code and 120 as 68k code.

**Status (2026-10-16):**

- **Done:**
  - The interpreter (`Sources/CPUPPC`), checked against hand-made vectors and a Retro68 probe (`Tests/Probes/PPC`) whose integer and floating-point results match the host's bit for bit.
  - The Code Fragment Manager (`cfm.c`): PEF sections and pattern data; relocations, repeats included, relative to each section's default address; imports and exports, weak imports, data imports.
  - Library search: the application's own file (data fork, or a resource compressed with the application's own 'dcmp'), then its folder.
  - Init routines run before main. GetSharedLibrary, GetDiskFragment, GetMemFragment, FindSymbol and the rest work, and accelerated code resources are prepared on their first call.
  - Mixed Mode (`mixedmode.c`) works in both directions: Pascal, C, register and dispatched procInfos; fat descriptors; CallComponentUPP.
  - InterfaceLib:
    - Generated glue calls the `tb_` functions, or runs the 68k trap itself (stack, register and selector conventions, trap bits).
    - LMGet/LMSet come from the low-memory definitions.
    - Hand-written glue in `ppc_glue.c` and `ppc_libs.c`.
  - MathLib, StdCLib (strings, memory, sprintf, setjmp, qsort), the Component Manager and movie controller calls, the Display Manager basics, and stubs for TSM, the Communications Toolbox, AppleTalk, ObjectSupportLib and Internet Config.
  - Gestalt answers as a PowerPC G3 with System 7.6.1 for PowerPC applications.
- **Also done:**
  - DrawSprocket of our own (`drawsprocket.c`): a back buffer GWorld at the size and depth asked for, centred in a black window over the whole screen, with the menu bar hidden.
  - InputSprocket on the keyboard (`inputsprocket.c`): needs are mapped to keys by label or name.
  - Display Manager mode lists.
  - Struct results through r3.
  - Gestalt as a G3 running 7.6.1, with Sound Manager 3.2.1.
- **Census:** of the 225 PowerPC and fat applications in the corpus, run as PowerPC, 167 reach their event loop (`--ppc`); of the 1,036 68k ones, 780 (2026-10-18). The corpus, the lists and the scripts are kept outside the repository (macOS's /tmp cleaner took an earlier copy); `run_census.sh <dir> <binary> <name>` runs both.
  - Of the 157 fat ones, 110+ reach it either way, and only a handful only as 68k.
- **Next:**
  - Report Mac OS 8.1 to applications that require it. That depends on M9's Appearance Manager and Navigation Services, because Setback, Spades, sixtyforce and Sound Studio check for them.
  - StarCraft's display check: Storm enumerates modes and still refuses 640×480×8.
  - Open Transport's libraries (M9).
  - The rest of the missing imports, ranked by the PowerPC census (`imports` column).
  - HyperCard 2.4's Practice stack sometimes leaves the background title off its second card, as PowerPC or 68k code alike.
  - A PowerPC oracle (SheepShaver with a New World ROM).
- **Status (2026-10-20):** the census's missing imports are in (Slot Manager, sound conversion, Carbon-era helpers, StdCLib file calls, the CursorDevice calls, and more), StarCraft's Display Manager modes carry their timing flags where Storm reads them, and the ContextualMenu library is ours. Census at the end of that day (Appearance and games merged, Word, PopChar and Shards fixed): 177 of 225 PowerPC runs and 790 of 1,036 68k runs reach their event loop.
- **Known:** Star Chaos and Smashing Windows II run wild in a VBL task. Space Debris spends its headless budget mixing sound in its double-buffer callback (the headless clock gives 100 ns an instruction).

### M9: Open Transport and the Mac OS 8/9 surface

**Goal:** a broad range of apps from the 7.5–9.2 era runs.

- **8/9-era managers:**
  - Appearance Manager (themes: Platinum by default, System 7 look optional)
    - from M1: Mac OS 8.5 Appearance themes; the Appearance controls (sliders, tabs, themed pop-ups); Kaleidoscope schemes' menus in screen mode and desktop patterns
    - Revisit the stopgap from 2026-10-13: `HiliteRGB` is set at boot from the host's selection colour (`useHostSelectionColor` in `HostWindow.swift`, `machine_set_hilite_color`). With the Appearance Manager, the theme's highlight brushes (`kThemeBrushPrimaryHighlightColor` and the selection brushes) should take the host's accent and selection colours, and `HiliteRGB` follow the theme (and change when the host's colours change, not only at launch).
    - Done (2026-10-20): theme brushes and colours, Platinum drawing, the host's highlight and accent colours (`appearance_poll`), Control Manager 2.0 and the Appearance controls, Mac OS 8's dialogs (items as controls, 'dlgx', 'dftb', AutoSizeDialog), MenuEvent and item modifiers, command IDs and 'xmnu'. Gestalt reports Appearance 1.1 (Mac OS 8.5's), with Icon Services there for the programs that take it to mean 8.5 (Greg's Browser, ZTerm). Kaleidoscope 1.x racing stripes draw their ends once per run.
    - Test case: Greg's Browser's list definition (LDEF 128) highlights a selected row only when Gestalt `appr` reports the Appearance Manager: it then calls `SetThemeBackground`/`SetThemeTextColor` (ThemeDispatch `$AA74`, selectors 2 and 3) and erases the row in `HiliteRGB`. Without it (System 7, today) it draws no selection at all. Its focus frame (an `InvertRgn` around the list) sometimes shows on an inactive list, which needs checking against activate events.
  - Navigation Services (native dialogs when no hooks): done (2026-10-20) on Standard File's host panels, the object filter consulted for each file; event and preview procedures are not called. PowerPC only (NavigationLib).
  - Contextual Menu Manager: done (2026-10-20), `$AA72` and PowerPC; no plug-ins or help item.
  - Icon Services: done (2026-10-20), PowerPC only (IconServicesLib, as on Mac OS 8.5): IconRefs for the system's icons, applications' bundles (by creator, through the desktop database), files' and folders' custom icons; registering, overrides, composites; 'icns' families with 48-pixel, 32-bit and 8-bit mask members, drawn, hit-tested and converted to and from suites. Gestalt `icon` says so. ControlsLib's 8.5 calls: 32-bit settings and the view size (kept, thumbs not yet proportional), for ZTerm. sixtyforce now passes its Appearance check and stops at its next requirement, Apple OpenGL 1.1.2 (OpenGLLibrary).
  - Window Manager 2.0 additions: from PowerPC code (2026-10-20): features, regions, collapsing, classes, `CreateNewWindow`, floating windows (WDEF 124's) passed over and hidden together, properties, content colour; proxies are accepted and not shown. Appearance 1.0's six (GetWindowFeatures, GetWindowRegion, the collapse calls) also reach 68k code through `$AA74` (2026-10-20). Floating windows stay in front of the others after InitFloatingWindows (2026-10-20). 8.5's calls for 68k (2026-10-20): none of Universal Interfaces 3.2 to 3.4.1's MacWindows.h gives them a 68k inline (only the six Appearance 1.0 calls are `$AA74` selectors there; the 8.5 ones are WindowsLib-only), so nothing was added; any 68k selectors would have to come from the 8.5 System itself.
  - Control Manager 2.0: done (2026-10-20), with the Appearance merge.
  - Thread Manager: cooperative threads are done (2026-10-13). Preemptive threads are still scheduled cooperatively, and a custom scheduler (`SetThreadScheduler`) is recorded but not consulted.
- **Drag Manager ↔ host drag and drop:** files and text in and out of bluebox apps. Into them: done (2026-10-20, `machine_host_drag`). Out to the host: done (2026-10-20, `machine_drag_out_*`, Sources/bluebox/DragOut.swift): a TrackDrag that leaves the machine's windows becomes an AppKit drag session in both rootless and screen modes, 'TEXT' as text, 'TEXT' with 'styl' as RTF, 'hfs ' as file URLs, 'PICT' as PNG and TIFF, 'phfs' as a file promise the sender writes; promised flavors come from the send procedure when a receiver asks. Text dropped on the Finder becomes a .txt file (the clipping). Brought back over a classic window, the drag is the program's again. TrackDrag returns noErr, userCanceledErr or the receive handler's answer, and GetDropLocation gives an alias to the folder a drop went to. Not yet: a drag image from the program's own (SetDragImage), the Trash as a drop location (programs that delete what is dragged to it), 'phfs' files keep the sender's name rather than the promise's.
- **Open Transport** (it comes after M8 because its API is delivered as Code Fragment Manager shared libraries, CFM-68K on 68k Macs):
  - endpoints, providers and notifiers; TCP, UDP and DNS on host sockets
  - AppleTalk through OT (ADSP, ATP, DDP, NBP) on the M5 AppleTalk stack, so OT-era games and apps share the same network: done (2026-10-20), with AppleTalk services and OTATalkSharedLib's NBP helpers. Since the same day: ADSP's attention messages both ways (T_EXDATA) and its EOM option (`adsp(EnableEOM=1)`), and pap and asp endpoints (ot_session.c), both ends of each, on host ATP: PAP's OpenConn, SendData with flow quanta, EOF as orderly release, tickles and CloseConn; ASP's OpenSess, commands as OTSndRequest/OTRcvReply and OTRcvRequest/OTSndReply, attentions, GetStatus and CloseSess (ASP's Write is not done). Data given as OTData chains is taken by every send.
  - the OT-era Gestalt selectors; apps that use both OT and MacTCP glue keep working
  - Exit check (2026-10-20), `scripts/ot-exit-check.sh` (headless; the applications come from outside the repository): over TCP/IP, Fetch 4.0.3 (PowerPC, OTInetClientLib) logs in to an FTP server on 127.0.0.1:2121 and lists a folder (given as `ftp://127.0.0.1:2121/pub/` in New Connection's host field; Fetch drops a `:port` after a plain host name). Over AppleTalk, two copies of the Shadow Warrior demo, set up by its SW Net Startup for an AppleTalk game, register and find each other through Open Transport's NBP mapper, exchange DDP datagrams on ddp endpoints (`--appletalk local`) and leave "Starting AppleTalk game…" for the game. What it took: data sent as OTData chains (Shadow Warrior's datagrams; its copies saw each other but heard nothing), and DNS addresses (`host:port`, OTInitDNSAddress) for OTConnect, as Fetch connects. Not yet seen: two Shadow Warriors in a level together (the intro wants a key press), and Shadow Warrior over TCP/IP (it needs another host's address). Still to look at: Internet Explorer 5.1.7 (PowerPC) quits right after MaxApplZone, before any Open Transport call; Spades Deluxe 2.0 (NetSprocket) runs with `--gestalt sysv=0x0810` but shows no Network menu unregistered.
  - Client interface completed (2026-10-28), the whole of Universal Interfaces 3.4's and Mac OS 9's OTClientLib, OTClientUtilLib, OTUtilityLib, OTInetClientLib and OpenTransportLib/OpenTransportUtilLib/OpenTptInternetLib client calls short of the STREAMS framework, the configuration database (OTCfig*, the TCP/IP and AppleTalk control panels' preferences) and the C++ provider internals: XTI (`t_open` ... `t_errno`) on the same endpoints; option management on the host socket (`ot_options.c`: keep-alive, reuse, TCP_NODELAY, TTL, TOS, broadcast, linger, buffers, multicast interface/TTL/loop/membership, the AppleTalk retry/interval/reply count/EOM ones; negotiate, check, default, current, T_ABSREQ, and the options a connect, an accept and a datagram carry) with `OTNextOption`, `OTFindOption`, `OTCreateOptions`, `OTCreateOptionString` and the option information calls (their signatures are inferred: Apple never published them); the port registry (`ot_ports.c`: the host's Ethernet-like adapters as ports "enet", "enet1" ..., registered ports, scan, state changes, user names, registered clients told of ports and of the stack loading); the Internet services on the host's resolver and network (`ot_inet.c`: `OTInetQuery` for any record type, mail exchangers, HINFO, address to name, interface information with the real mask, gateway, name server, MTU and hardware address, secondary addresses, the DHCP lease's options through `ipconfig` on macOS); the utility calls (`ot_util.c`: bit maps, hash lists, gates, queues, time stamps, random numbers, interrupt level, port references, no-copy OTBuffer receives); expedited data as TCP urgent data; `OTAlloc` for every structure type; `OTOpenProvider`. 68k code: Apple's glue linked into 68k programs reaches the same implementation through a stand-in for the Shared Library Manager (`ot_68k.c`: the Expand Memory record's manager global, the dispatcher that binds each stub to a native routine by the glue's own class and index table, `ot_68k_table.c`), so 68k programs (RumorMill, Easy Serve) and the 68k code in PowerPC programs see Open Transport; Gestalt no longer hides it. Raw streams (OTNativeClientLib, the glue's NtvClient set; `ot_streams.c`): streams by driver name or configuration, the STREAMS head's ioctls (push, pop, look, list, find, flush, read and write modes, signals, peek, bands, close time), getmsg/putmsg with bands and priority, read/write, polls, pipes, OTBuffer reads and put-backs, notifiers, asynchronous completions, the state machines; echo and nuls behave as Mentat's drivers, transports answer T_INFO_REQ and refuse other TPI requests, "sad" keeps autopush lists, ip's named parameters (ND_GET/ND_SET) come from the host; modules other than the ones Open Transport shipped can't be pushed (a program's own STREAMS module, as OTSessionWatcher, StreamWatcher and IPNetMonitor's monitor load, is not loaded). Configurations (`ot_config.c`): the OTCfig* tree, OTConfigureChildren down to the port, configurators and control streams; OTConfigLib's port calls. Left out: `OTIoctl`/`OTGetMessage` on endpoints beyond I_NREAD and flushes, the configuration database (OTCreateConfigurationsDB ...), the C++ provider internals (the mangled names in OpenTransportLib and the glue's TNativeProvider set), the AppleTalk preferences calls, and kernel-only registrations.
- **Apple Events:** a broader set, plus a host-side "do script" hook.
  - Done (2026-10-20): records under any descriptor type ('obj ', 'rang' and the rest), arrays, coercion handlers (the application's and the system's, pointer and descriptor) before the standard coercions (numbers, text, Boolean and enumerators, FSSpecs and aliases, one-item lists); dispatching with the predispatch handler and both handler tables, replies with keyErrorNumber, queued replies as Answer events, AESuspendTheCurrentEvent/AEResumeTheCurrentEvent, AEInteractWithUser by source and level; the Object Support Library's AEResolve (nested containers, lists of tokens, ranges, whose tests evaluated with the count and compare callbacks, token disposal) and the CreateObjSpecifier family, for PowerPC from ObjectSupportLib. Between runs: each listens on a Unix socket, the Process Manager lists the others after the application (by signature; their names stay empty), and AESend reaches them by signature, serial number, process ID or target ID. From the host: `machine_send_apple_event`, `bluebox send` (odoc, pdoc, oapp, quit, dosc, any class and ID), and in the macOS host Quit Application, Print Documents (then Quit when launched to print) and Do Script with a scripting dictionary in the bundles. Checked with SimpleText and BBEdit 2.1.3 (documents sent to the running program open; quit ends it).
  - Still to do: other runs' names in GetProcessInformation; marking callbacks (kAEIDoMarking); kAEIDoWhose gets the test record unconverted rather than a whose descriptor; forwarding events from macOS other than the core ones and Do Script (object-model events from AppleScript); idle and filter procedures while AESend waits; checking osascript against an installed bundle (it needs the automation permission). BBEdit 2.1.3's text area draws black, with or without Apple Events.
- **Corpus expansion:** office, graphics and internet apps, with a quirks database.

**Exit:** a defined 8/9 corpus (to be chosen at M8) passes its smoke tests, and an Open Transport app (a browser or an OT-based network game) connects over TCP/IP and over AppleTalk.

**Status (2026-10-20): done.** The 8/9 corpus is every PowerPC application in the census corpus importing one of Mac OS 8 and 9's libraries (Appearance, Navigation Services, the Window, Menu, Control and Dialog libraries, Contextual Menus, Icon Services, Open Transport, the Text Encoding Converter, the Sprockets), less Sigma Chess's endgame generators (batch tools) and Spot Box's Yomavaders (a launcher for QuickTime Player): `scripts/smoke-m9.sh` runs them headless as PowerPC, each in a home of its own, for up to 2 billion instructions. Fetch 4.0.3 over TCP/IP and Shadow Warrior over AppleTalk pass `scripts/ot-exit-check.sh`. Fixed on the way: PowerPC programs see Mac OS 8.1; Icon Services, with Appearance 1.1; asynchronous PB calls from PowerPC run their completions (Squirt Derby!, DroidWorks); DrawSprocket's alternate buffers, blits and one-page contexts; Open Transport's T_ORDREL told before kOTLookErr (Internet Explorer 3's "attempt to load failed"). Carried to M10 (compatibility): sixtyforce wants Apple OpenGL 1.1.2; Internet Explorer 5.1.7 quits in its memory library after MaxApplZone (also M8's); Star Wars DroidWorks plays its crawl but its timed intro runs past the smoke budget; Spades Deluxe 2.0 shows no Network menu unregistered.

### M10: Performance and games

**Goal:** heavy apps and games run at a usable speed.

- **JIT** for 68k and PPC on macOS (`MAP_JIT`) and Linux.
- **Persistent translation cache** keyed by a hash of each code resource or fragment, with an ahead-of-time "pre-translate" option in `bluebox install`. The interpreter remains the fallback.
  - The approach (2026-10-20): the JIT and the cache are built together. Blocks are translated on first execution and kept on disk under the hash of their code resource or fragment and their offset, so a second launch starts with everything the first one ran. Translation ahead of time only adds to that: `bluebox install` walks from the entry points (PowerPC fragments first: fixed-width code in known sections, with PEF's relocations; 68k code is harder to find statically, with jump tables, A5-relative calls, and data among the code). Code made or changed at run time (blitters copied into the heap, decrypted code, code resources from other files, self-modifying code) goes through the JIT or the interpreter, so the fallback stays.
  - Ahead-of-time gives no speed beyond the JIT's in the steady state (the same translator); it removes the warm-up, and leaves room for heavier optimisation offline: registers kept across blocks, and 68k condition codes not computed where nothing reads them (the biggest single win).
  - **Built (2026-10-21, behind `--jit` / `machine_set_jit`, off by default; arm64 hosts).** A block engine in CCore (`jit.h`, `jit.c`, `jit_a64.h`, `a64.h`): executable memory (MAP_JIT with `pthread_jit_write_protect_np` and `sys_icache_invalidate` on macOS, `mprotect` W^X elsewhere), blocks keyed by guest address, exits chained once taken, indirect branches looked up inline, and every block entered with its length taken from the instruction budget, so `icount` (the machine's clock) and the PowerPC time base stay exactly as the interpreter keeps them. Every store into guest memory (the CPUs', the Toolbox's, the bulk copies) checks a map of 256-byte regions holding translated code; a store drops only the blocks it overlaps, and a store into the running block makes it leave at the next instruction. icbi and MakeDataExecutable drop their range too. Translated code is position-independent, so it is the persistent cache: `~/Library/Caches/Blue Box/JIT` (or `--jit-cache`), a file per application and CPU, a record per block holding its address, the hash of its guest code (and of the code its flag analysis looked at) and the host code, used only by the build that wrote it.
    - PowerPC (`ppc_jit.c`): integer arithmetic, compares, rotates, loads, stores and branches translated; floating point, string and multiple transfers, lwarx/stwcx. and CR logic run inside the block through `ppc_execute`; sc, NATIVE, traps and the time base end blocks. The five GPRs a block uses most stay in host registers. CR fields set and overwritten within a block are not stored, and a branch right after its compare uses the host flags.
    - 68k (`m68k_jit.c`): the common integer instructions in every addressing mode but the 68020's memory-indirect ones; bit operations, register shifts, ADDX, CCR moves, long multiply and divide, CHK, BCD and bit fields run inside the block through `cpu_execute_at` (an exception leaves the block); A-line traps, NATIVE, TRAP, RTE and the FPU end blocks. Condition codes are computed only where read: within the block, and after it as far as its successors' first 16 instructions show (X always kept); a Bcc, DBcc or Scc after the instruction that set its flags branches on the host's. Between blocks, SR's N Z V C may be stale where the following code sets them before reading them, and op_pc can name an earlier instruction after a chained exit; neither is read there.
    - Correctness: the PowerPC vectors and the 68000 single-step vectors run through the JIT too; random blocks run by the interpreter and the JIT from the same state are compared (registers, CR/SR, memory; for the 68k also chains of blocks with the flag lookahead on); stores into the running block are tested. `swift test` passes with the JIT off by default. Census (2026-10-21, 60 million instructions each): all 227 PowerPC programs stop the same way with and without `--jit`; of 1,038 68k programs, 5 differ, each one that isn't repeatable from run to run without the JIT either (they seed from the date) or shares its preferences with another copy in the census home.
    - Speed (`scripts/bench-jit.sh`, 600 million instructions headless, release build, M-series host): PowerPC Squirt Derby! 2.05 s → 0.20 s (10x), Sigma Chess's KQKR and KBNK generators 2.4–2.7 s → 0.21–0.24 s (11–12x), Sirtet Voyager (in its event loop, mostly Toolbox) 1.02 → 0.60 s; 68k Squirt Derby! 3.70 → 0.43 s (8.6x), Safari 1.0 4.10 → 0.73 s (5.6x), Shards 5.8 → 5.1 s (its time is the census bookkeeping and network polling in a TickCount loop, not the CPU). The cache saves only the translation time (a few ms) on a second run.
  - **Left:** CR lookahead across PowerPC blocks (as the 68k's flags); registers kept across blocks; ahead-of-time translation in `bluebox install` (PowerPC fragments first), keyed by fragment or code resource and offset rather than by address and path (the cache misses when code loads elsewhere or the application moves); an x86-64 back end; translating floating point (both CPUs) instead of calling the interpreter; the 68k's memory-indirect modes; the census's per-trap bookkeeping and the network polling on every TickCount, which now dominate loops that wait on the clock; turning the JIT on by default once it has run more of the corpus interactively.
- **Games:** InputSprocket → host gamepads, DrawSprocket → screen mode, and Sound input. Done (2026-10-20): InputSprocket on the keyboard, the mouse and up to four host game controllers (GameController.framework), DrawSprocket's context selection, and the Sound Input Manager on the host's microphone (SPB recording, SndRecord's dialog). Fixed the same day from the census: Star Chaos (disposed windows kept whole a while), Squark (depth-0 GWorlds), Safari 1.0 (the 68040's FSAVE frame), Bad Day on the Midway (case-blind catalog order, QuickTime time arithmetic), Prince of Persia 2 (drives in the drive queue, MemoryDispatch), the Star Trek demo (GetCTable 72), Squirt Derby! (SetupSndHeader), Space Madness (SaveEntries' request list), Shufflepuck Café (LoadSeg through a patched GetResource), Skittles 2, ShatterBat and StarCraft's Startup Options. StarGuardian's "rabbit" is a random-dot stereogram: not a bug. Solitaire of Champs fails only on its first run: it calls UseResFile(0) with a resource file it never opened, then finds no name for its preferences (the System file alone is searched, as on a Mac).
- PowerPC programs the census saw busy at 60 million instructions (2026-10-20): Secret Agent Joe, Sirtet Voyager, SpaceDeubza 1.01 and Splash! reach their event loops within 600 million, drawing as they should (Sirtet Voyager wants 256 colours and relaunches); their intros are only long. Starry Night 1.0.1 quits only to relaunch at 256 colours, then runs; its registration dialog's radio buttons (Grayscale Buttons) lost their labels to a black control body until controls without colours of their own shared the standard table. StarGuardian is the HyperCard standalone under M7.
- CFM-68K (2026-10-21): 68k code in PEF containers, loaded by the 68k Code Fragment Manager (`_CodeFragmentDispatch`, $AA5A), is not supported: Internet Explorer 4.01's 68k build is one (its CODE resources only ask CFM-68K to load the program). Its fat installer installs the PowerPC build now that installers are told they run on a PowerPC; CFM-68K itself waits for a program with no PowerPC build.
- Mac OS 8's Internet Setup Utility (Apple's SOM runtime and its Internet Access library) and Mac OS Setup Assistant (AppleScript, the File Sharing library, a network settings file it looks for) set up a real Mac's dial-up, TCP/IP and file sharing, which Blue Box takes from the host: not planned. Setup Assistant used to crash Blue Box with a nil TEHandle; TextEdit ignores those now.
- Apple's Text Encoding Converter extension (2026-10-21; Internet Explorer 4.01's installer adds it): its libraries find their globals through a table its boot-time code registers (TextCommon reads it through the international resources), which nothing here runs, so its UnicodeConverter looped without end and Internet Explorer 4 and 5 hung at their splash screens; the Code Fragment Manager then left the converter unloaded, and both said it was missing. The converter is ours now (2026-10-21, `tec.c`): TextCommon, UnicodeConverter and TextEncodingConverter as TEC 1.3.1 had them (TextEncodings and script codes, Unicode converter infos and text runs, TEC converters with a character cut short kept between calls, one-to-many converters, Internet charset names, the available, web and mail encodings, a sniffer), Unicode's forms converted here and every other encoding on the host (`hosttext.c`: CoreFoundation on macOS, iconv elsewhere); only TextCommonGlobals stays unloaded. Internet Explorer 4.01 and 5.1.7 no longer say it is missing and reach their browser windows; 4.01 shows a Mac OS Roman page through its converters (to UTF-8 and back in runs). 5.1.7's "Insufficient memory" error at its first page is there with the converter missing too. Left: a program's own Unicode fallback handler (SetFallbackUnicodeToText) is accepted but not called; stateful encodings (ISO-2022, HZ, UTF-7) convert a buffer at a time, not across calls; the sniffer is a heuristic (bytes that aren't characters, multibyte characters found); Unicode mapping queries (CountUnicodeMappings, QueryUnicodeMappings) and the TEC plug-in interface are not there. TEC 1.3.1 registered no Gestalt selector, so neither do we.
- International resources (2026-10-21): the System file has U.S. `itl0`, `itl1` (with its Itl1ExtRec), `itl2`, `itl4`, `itlb` and `itlc` (Generators/intlres.py, Inside Macintosh: Text appendix B), the MacRoman tables derived from Unicode (case, diacritical marks, character types) and the word and line break state machines written from the documented classes and states. Compared with Mac OS 8.1's: itl0 and itl1 agree byte for byte; itl2's case, strip and type lists for every character, its word-break classes and machine (on random text) and itl4's token map, untoken table, number parts and white space agree; deliberate differences are a run of hyphens or operators kept together for line breaks, a backward word table that always lands on a boundary, filled-in itl2 lengths, no Mac OS 8 extension after itl2's tables, and itlb's local size and system font (0, Chicago). `GetIntlResourceTable` returns the break tables, number parts, untoken table and white space list from them; CharacterType, UppercaseText and the other case and mark conversions, the IUMagString family and FindWord/FindWordBreaks (`breaks` nil, -1 or a program's own NBreakTable) now follow the tables; UprString, RelString and EqualString keep the original character set's rules. Fixed with it: timeFmt's leading-zero bits, the long date's order (lngDateFmt and suppressDay) and the fallback itl1's field offsets; PowerPC `GetHandleSize` and `GetPtrSize` answer 0 for a bad handle (they gave the result code, and Internet Explorer 5.1.7 sized a copy of a nil handle by it, the "Insufficient memory" alert after its window opened: it now loads its home page). Still to do: 'itlm' (sorting by script and language), 'itl5', the itl1 extension's abbreviated names in IUDateString, StyledLineBreak on the line-break table, and IntlTokenize (not provided).
- CarbonLib is deferred: Super Snake is the corpus's only Carbon application (2026-10-20), so a full CarbonLib waits for a reason beyond it.
- Internet Config's ICLaunchURL (68k component and PowerPC library) opens a URL in the host's browser, or prints it on headless runs (2026-10-20): Skittles 2's home page launcher.
- Compatibility carried from M9 (2026-10-20): OpenGL (below); Internet Explorer 5.1.7's early quit in MS Memory Library (it now runs to its home page, below); Star Wars DroidWorks past its crawl (and with input); Spades Deluxe 2.0's NetSprocket Network menu.
- OpenGL (2026-10-22): Apple's OpenGL for Mac OS 9, PowerPC only: OpenGLLibrary (its gl calls as 1.2.1 had them, with ARB_multitexture, ARB_transpose_matrix, compiled vertex arrays, EXT_texture3D and the blend extensions, and AGL) and OpenGLUtility (GLU's matrices, projection, mipmaps, image scaling and quadrics), on the host's OpenGL (`opengl.c`: OpenGL.framework, a legacy 2.1 CGL context per AGL context, current on the emulator thread). Calls go straight to the host's; arguments come by the PowerPC convention (floats in f1 on, each taking a general register's slot, past r10 on the stack), arrays of shorts, ints, floats and doubles are byte-swapped, pixels go to and from guest memory with the host swapping their bytes (GL_UNPACK/PACK_SWAP_BYTES kept the opposite of the program's), vertex arrays are converted at each draw (glArrayElement becomes immediate-mode calls), selection and feedback buffers are copied back by glRenderMode. A context renders into a framebuffer object the drawable's size; aglSwapBuffers (glFlush single-buffered or drawing to the front) reads it back once into a 32-bit pixel map in the system heap and CopyBits it into the window (`agl.c`), so it shows at any screen depth, clipped by the window's visible region. AGL: pixel formats, one accelerated renderer (32 MB), contexts sharing lists and textures, window, off-screen (aglSetOffScreen) and full-screen drawables (a black window, the frame scaled to fill it), AGL_BUFFER_RECT and AGL_SWAP_RECT, aglUseFont (QuickDraw's glyphs as bitmap lists), errors and their strings; aglGetVersion says 2.0. No Gestalt selector (Apple's had none; sixtyforce looks for the weak imports and calls aglGetVersion). glDrawPixels draws untextured whatever texturing is on, as the era's renderers did (sixtyforce's frame rate counter). Linux has no OpenGLLibrary yet (programs see it missing). sixtyforce 0.5.5 (the corpus's only OpenGL program) reaches its event loop; with public-domain ROMs (from github.com/DerekTurtleRoe/N64-PD-ROMS) it renders through OpenGL in its Video window: Funnel Cube's game board and text, at about 100 frames a second headless. Fogworld and the 3DS Model Conversion demo show only their text: sixtyforce's own viewport for their 3D puts it outside the window (y 240 of a 240-high drawable), not a GL fault. Left: colour-index mode, accumulation buffers (a framebuffer object has none), the imaging subset (glColorTable, convolution, histogram), GLU's tessellators and NURBS, aglSetFullScreen changing the display's resolution (the frame is scaled instead), and sixtyforce's Video Settings dialog, which draws empty (its filter takes the dialog's update event with BeginUpdate and EndUpdate before ModalDialog can draw it).
- Drawing problems seen in the census (2026-10-13; status 2026-10-22):
  - StarGuardian (HyperCard 2.x standalone): the program's own. Its AddColor XCMD converts its parameters to Pascal strings with a signed length byte, so a string of 128 characters or more (here a full pathname) makes its byte move count about 4 GB and it spins there with the card unpainted. Run from a short path it reaches "Click on rabbit to continue" (the rabbit is a random-dot stereogram, not a bug). The Host folder makes full pathnames longer than on a Mac (Macintosh HD:Host:Users:…: the census corpus path is 142 characters), so a deep host folder is enough; showing granted folders nearer the root would avoid it.
  - Sword of Ages (World Builder): fixed by later work; the start window and the scenes draw.
- Redraw check (`--redraw-check`, 2026-10-16) over the 665 applications that reach their event loop: 409 redraw identically, 254 differ, 211 of them on screens that do not change on their own. Many are programs that draw once and never handle updates (DOCMaker's font dialog: a real Mac blanks those too), so each needs judging against the oracle. Candidates worth a look first: Greg's Browser's column headers keep their inactive (grey) look after the window activates; the lavender sliver above its list's selected row; SimStapler, Sim Tape Dispenser, SuperBike, RoboWar, MacPaint 2.0 (small, static differences).
- DOCMaker documents (SKELETON GOODIES and others): their pictures (extended version 2 headers) show since the native frame is read from the right place (2026-10-16).
- Census changes from 2026-10-16 (status 2026-10-22):
  - Star Chaos 2.0.1: fixed (disposed windows kept whole a while); its title screen shows.
  - Squark: fixed (depth-0 GWorlds); its Soleau logo intro runs about a billion instructions headless, then the board shows.
  - Bad Day on the Midway: fixed. It finds its CD, plays its QuickTime intro, and its scenes now draw: the projector spools PICT files in through a getPicProc (DrawPicture gets a handle of just picSize and picFrame), which DrawPicture now reads through (2026-10-22); the fortune teller's booth was a white rectangle.
  - Prince of Persia 2: fixed (drives in the drive queue, MemoryDispatch); its intro plays at 256 colours.
  - Squirt Derby!: resolved (M9).
  - Space Madness Demo: fixed (SaveEntries' request list); its title and high scores show.
- Census follow-ups from 2026-10-18 (fixed that day: StuffIt InstallerMaker installers reading their own resources through the File Manager, Solitaire Till Dawn's Gestalt OS check and its splash dialog erased by a hidden window, Slabbers' CalcMask overrun, Steel Fighters' GetSoundOutputInfo, Spot Box's StandardAlert, Shockwave's NumVersion result, ZTerm's phone book, Sail Away and Space Joust needing 256 colours, the Star Trek demo's cache sized from too much memory; status 2026-10-22):
  - Microsoft Word 5.0: fixed (OSDispatch selector 2 is MultiFinder's GetSwitchInfo); it reaches its registration dialog.
  - PopChar Pro's installer: fixed (ReadPartialResource reads the file, so its checksums match). Its static texts were cut off: its 'ictb' sets them in Geneva 9, and item colour tables now set text items' font, face and size (2026-10-22).
  - Safari 1.0: fixed (the 68040's FSAVE frame); it reaches its board.
  - Shards: its title's flying pieces left trails of copies; they erase through regions made with OpenRgn from their edges, and lines drawn while a region is open now bound it (2026-10-22). Its window (600×260 at 20,50) comes from its own 'puzl' data; the title's letters end near the window's bottom edge, which may be its own layout (not checked on the oracle). The intro still takes several hundred million instructions headless.
  - Warlock: Total Distortion demos (Director 4.0.4 projectors): black screen, still open. The stage window uses the movie's own WDEF 31, "WindowHider Universal 1.0" (Andrew Green), which covers the screen and computes the content region from the window's portRect offset by the words at window+8 and +10, portBits.bounds in a GrafPort but grafVars in the CGrafPort GetNewCWindow made; the content lands at h −16404, the visRgn is empty and every CopyBits to the stage is clipped away. It also patches MoveWindow to call SizeWindow after it. Best lead: what a real Mac's grafVars or WindowHider path gives here (compare on the oracle); FreakShow, a Director 4 projector without WindowHider, draws.
  - Shufflepuck Café 1.0: fixed (LoadSeg through a patched GetResource); it reaches its bar.
  - AppleScript applets (the Stupid AppleScript Games, ScanTastic's scripts): they run now, on our own AppleScript (M14, 2026-10-23); the games play headless with their dialogs' defaults.
  - The Star Trek demo: fixed (GetCTable 72); at 256 colours it reaches the bridge and the captain's log.

**Exit:** a defined game corpus runs at full original speed or better, including QuickTime playback from M4 at full frame rate.

**Status (2026-10-22): done.** The game corpus is `scripts/games-m10.sh`'s 18 games (ten 68k: Glider+, Shufflepuck 1.5, Prince of Persia, Spectre, Bolo, Crystal Quest, Solarian II, SimTower, Snood, Star Chaos; eight PowerPC: Squirt Derby!, DroidWorks, Sirtet Voyager, StarCraft, Tetris Max, Snood, Spades Deluxe 2.0, Star Chaos): all 18 keep running and execute faster than the Mac they were written for (a 40 MHz 68040, 35 million instructions a second; a 400 MHz G3, 600 million). The 68k games do so interpreted (52 to 1,440 million a second); two PowerPC games need the JIT for a G3's speed (Squirt Derby! 297 million interpreted, 3,488 with the JIT; StarCraft 467 and 646), the others manage it either way. SimTower, the slowest, went from 37 to 52 million when CopyBits stopped searching colour tables at every pixel. `bluebox movie-speed` decodes 545 of the corpus's 547 movies at 10x real time or more (most over 1,000x; two use a codec only VLC plays). sixtyforce draws through OpenGL. The JIT stays off by default, as a setting (`bluebox set … jit on`, the shell's Machine section) until it has had more interactive use; for PowerPC games that want a G3's speed it should be on. Spades Deluxe 2.0's missing Network menu is its own: it deletes the menu while its preference 'PREF' 20016 is 1, its default until registered (NetSprocket's OTList and OTInetStringToHost calls work now). What's left moved to M12 (the x86-64 JIT back end, Linux OpenGL) and to the backlog below.

### M11: Native 64-bit SDK

**Goal:** new software can be written against the classic Toolbox as native 64-bit host code.

- A C header and library exposing the Toolbox through opaque types and accessors (Carbon-style), in host byte order with 64-bit pointers. `NewPtr` and `NewHandle` are backed by host `malloc`.
- Toolbox-owned objects stay in the guest arena behind opaque references. Bulk data is passed by host pointer.
- Generated from the same definitions as the 68k and PPC glue.
- Swift and C sample apps. Native apps use the same install path (`.app` bundle and `.desktop` file).
- **Native code inside the environment (arm64 and x86_64):** besides standalone native apps, native code compiled for the host's own architecture can run *in* a Blue Box session next to 68k and PowerPC code: a native application loaded like a classic one (its own "fragment" architecture beside `CODE` and `cfrg`, so the shell lists it with 68k, PPC and fat apps), and native plug-ins, code resources and shared libraries that classic programs call (as CFM did for PowerPC). Calls between native and emulated code go through the same Mixed Mode routine descriptors and generated glue; the Toolbox calls they make are direct host calls with no emulation. This is also the path for rewriting slow parts of classic programs (an XCMD, a codec, an MDEF) natively.
- Later option: WebAssembly (64-bit memory) as a portable, sandboxed format calling the Toolbox through imports.

**Exit:** a native sample app (a text editor and a drawing app) uses the classic UI, speech and printing, and allocates more than 4 GB.

---

### M12: Linux parity

**Goal:** everything that works on macOS works on Linux. (Moved here from M2: the macOS host comes first.)

- **Linux host (HostGTK):**
  - GTK4 rootless windows with per-window Mac coordinates
  - moves and resizes through `gdk_toplevel_begin_move`/`gdk_toplevel_begin_resize`
  - menu bar in each window, `xdg_popup` menus
  - pasteboard, native file dialogs
- **Linux launching:** `bluebox install` on Linux produces a `.desktop` file, a PNG icon and shared-mime-info XML.
- ~~**Our own HFS+ reader**~~ done 2026-10-16 (`Sources/ForkStore/hfs.c`: HFS+ and HFSX, bare or in an HFS wrapper, behind a partition map; Unicode names to MacRoman; hard links).
- **CI:** Linux CI, headless probe runs on both platforms.
- **From M10:** the JIT's x86-64 back end (Intel Macs and Linux), and OpenGL on Linux (Mesa) for OpenGLLibrary.

**Exit:** the applications that run on macOS run on Linux under both Wayland and X11, installed as desktop applications.

**Status (2026-10-12):** the core, the headless runner and the census build and run on Linux; there is no windowed host yet.

### M13: Apple menu items of our own

**Goal:** the everyday Apple menu items, written from scratch to look and behave like Apple's classic ones (no Apple code or resources), so every run's Apple menu has them without a System disk.

- Calculator, Chooser (printers through the host, AppleTalk zones once M5 is in), Jigsaw Puzzle (the classic map puzzle, and a picture from the clipboard), Key Caps (the current font and keyboard layout, Option and dead keys), Note Pad and Scrapbook (kept in the per-user System Folder, Scrapbook through the Scrap Manager), SimpleSound (alert sounds, recording once Sound Input works), Stickies, Alarm Clock.
- Built as desk accessories or small applications with our own Toolbox calls (68k code from our toolchain, or the native SDK of M11), installed in the System Folder's Apple Menu Items, where the Apple menu lists them and opens them.
- Their look checked against the oracle: window kinds, fonts, icons and behaviour as in System 7.5.

### M14: AppleScript

**Goal:** AppleScript works as it did in System 7.5–9: applets and droplets run, Script Editor compiles, runs and records, and scripts drive classic applications through Apple Events (and, through a bridge, the host).

- **The Open Scripting Architecture:** the generic scripting component and the AppleScript component (`osa `/`ascr`) through the Component Manager: `OSACompile`, `OSAExecute`, `OSACompileExecute`, `OSALoad`/`OSAStore`, `OSADisplay`, `OSAScriptError`, contexts, `OSASetActiveProc`, the send and create procs, and `OSADoScript`; `OSAGetSource` for decompiling.
- **The language, our own implementation:** a parser and interpreter for AppleScript 1.x syntax (tell blocks, handlers, script objects, properties, lists and records, coercions, `considering`/`ignoring`, `try`/`on error`, `with timeout`, `using terms from`), with the dictionary terms of each target application read from its `aete` resource (and the System's and the scripting additions' `aeut`/`aete`), and object specifiers built with the Object Support Library's conventions.
- **Compiled scripts:** the `scpt` format that applets, droplets and Script Editor files store (resource `scpt` 128 in a file of type `APPL` and creator `aplt`/`dplt`, or `osas` files), read and written so that scripts compiled on a real Mac run here and scripts compiled here run on a Mac. The format is undocumented: work it out against the oracle (compile known sources in AppleScript 1.1 and 1.3 and compare), decompiling where a script's source text is not kept.
- **Applets and droplets:** the applet shell (open, run, idle and quit handlers, the startup screen, Stay Open), and the `odoc` handler for dropped files, so the AppleScript applets in the corpus (the Stupid AppleScript Games, ScanTastic's scripts) run instead of quitting.
- **Scripting additions:** the Standard Additions people relied on (`display dialog`, `choose file`, `the clipboard`, `current date`, `do shell script` mapped to nothing or to a host bridge by choice), implemented natively rather than loading Apple's `osax` files; third-party `osax` code resources loaded and called like other code resources.
- **Recording:** Apple Event recording (`kAEStartRecording`, the recording event handler) so Script Editor's Record button works with recordable applications.
- **Host bridge (optional, off by default):** a scripting addition, or `tell application` to a host app, sent through the host's own scripting (osascript on macOS), for classic scripts that want to reach the modern system.
- **Gestalt and folders:** `ascv` (AppleScript version), the Scripting Additions folder, and the Scripts folder's items in the Apple menu.
- **macOS scripting classic programs:** each program's bundle accepts any Apple Event and passes it into the program's queue (descriptors as they are, text between MacRoman and Unicode, replies back), and carries the program's `aete` in a `.rsrc` in its Resources, so Script Editor on macOS shows its dictionary and `tell application "SimpleText"` works.
- **Classic programs scripting macOS:** events a classic program sends to a process that isn't a Blue Box run go out through `AESendMessage`, but only to the applications the user allows (an allow-list in the settings, empty by default): an old script telling the Finder to empty the trash must not reach the real Finder unasked.
- **Status (2026-10-23): our own runtime runs compiled scripts; nothing runs on the host.** `applescript.c` reads AppleScript 1.1's FasLoad format ("FasdUAS 1.101.10", the 'scpt' resources of the corpus's applets; n0kovo's applescript-decompiler, MIT, as the reference for the format and the opcode names) and runs the bytecode on a stack machine of its own: handlers with positional and event parameters, properties, globals and locals, lists, records, text, integers and reals, booleans, dates, constants, the comparisons (text ignoring case unless `considering case`), arithmetic, concatenation and coercions, repeat (times, while, until, from/to/by, in a list) with exit, if, try/on error with its variables, error, tell, considering, timeouts, and object specifiers on the script's own values (item, character, word, paragraph, text item with the delimiters, every, some, middle, ranges, beginning/end, properties such as length, rest and reverse). What it doesn't have (script objects, whose filters, labelled handler parameters, compiling source) is a script error (-2700), never a crash or a hang; a running script counts towards the instruction limit and the machine's clock. `as_osax.c` has the scripting additions, natively: display dialog (buttons, default button, icon, default answer, giving up), choose from list, beep, random number, say (the Speech Manager), offset, count, current date, delay, choose file (Standard File), open for access/read/close access/get eof on the guest's files, Jon's Commands' clipboard (the guest's scrap), and Gestalt for the Finder's `computer`. Dialogs are the host's (`machine_set_script_dialog_handler`: NSAlert with a text field or a pop-up; headless runs take the default button and answer and log them, or the answers `BLUEBOX_SCRIPT_ANSWERS` gives). do shell script, open location and mount volume are refused; `tell application` reaches the program itself and other Blue Box runs by signature (`aem_script_send`, which never goes to the host; the Finder isn't a run, so only `computer` answers there), anything else is -600. `osa.c` registers the AppleScript component (`osa `/`ascr`, `osa `/`scpt`; OSALoad, OSAStore of the data loaded, OSAExecute, OSADisplay, OSAScriptError, OSADispose, OSALoadExecute, OSAExecuteEvent, OSADoEvent, the procs kept; OSACompile, OSACompileExecute and OSADoScript give a script error, "can't compile yet"; the PowerPC AppleScriptLib calls go to it), Gestalt `ascv` (1.1) and `ascr`, and the applet shell (`aplt`/`scpt`, selector 1 of the applet stub): the startup screen (TEXT 1128), run on Open Application, the open handler on Open Documents, Stay Open ('pref' 128 bit 0) with idle and quit handlers. The nine Stupid AppleScript Games run headless (dialogs answered with their defaults) until the instruction limit or their own end (Blackjack plays until out of chips); the ScanTastic and Automated Tasks applets show their startup screens and stop at their first `tell` to an application that isn't a Blue Box run.
- **Next:** properties saved back into the applet ('scpt' 128) when it quits, as AppleScript does (the games keep their scores there); our own FasLoad writer for OSAStore of changed scripts; script objects and labelled parameters; whose filters and the rest of the object model for scriptable Blue Box runs (and the Finder's suite on the guest's files, so the Automated Tasks work); the menu bar of a stay-open applet; then the compiler (`OSACompile`, Script Editor) and decompiling (`OSAGetSource`).

**Exit:** the corpus's AppleScript applets run; Script Editor (from a System 7.5 disk image) compiles, runs, records and saves a script that drives the Finder-free parts of a scriptable application (SimpleText or Scriptable Text Editor); a script compiled on the oracle runs here with the same result, and one compiled here runs on the oracle.

### M15: Self-contained application bundles (macOS)

**Goal:** a classic application packaged as a macOS application that stands alone: one `.app` holding the classic program, its settings and Blue Box itself, which can be moved, copied to another Mac, or signed and handed to someone. After M12 (asked for 2026-10-20).

- **What exists:** `bluebox install` already makes a launcher bundle in `~/Applications/Blue Box`. It has the program's icon (from its `ICN#`/`icl8`), its document types (`CFBundleDocumentTypes` from the `BNDL`/`FREF`), and an executable script that runs the library's copy with the installed `bluebox`, so documents dropped on it open in the program. It depends on both staying where they are.
- **Self-contained:** `bluebox install --standalone` copies into the bundle the classic program (both forks, AppleDouble or a small disk image), any folder it needs beside it (data files, a CD's contents), the run's settings (architecture, depth, scale, appearance, display mode, Gestalt overrides, the Internet settings), and the Blue Box executable, instead of a script that points at them.
- **Nothing written inside:** the program's System Folder, preferences and saved documents go under `~/Library/Application Support/Blue Box/<bundle identifier>`, so the bundle stays unchanged and its signature valid.
- **Signing:** ad hoc by default; a developer ID and notarization when the user has them, with the steps documented.
- Apple software and third-party programs stay out of the repository: bundles are built from the user's own copies.

**Exit:** a self-contained 68k bundle and PowerPC bundle each run from a different folder and on a second Mac without Blue Box installed, open documents dropped on them, keep their preferences across launches, and show their own icon and name in the Dock and the menu bar.

### M16: The Translation Manager and document translators

**Goal:** Macintosh Easy Open's Translation Manager, so classic programs open documents of other types through translators, and DataViz's MacLinkPlus 9.0 (Mac OS 8's Apple Extras: Document Converter, DataViz FileView) converts old documents (ClarisWorks, Word 5, MacWrite, WordPerfect and the rest) as it did. Asked for 2026-10-22, when Document Converter said Easy Open must be in the Extensions folder.

- **The Translation Manager:** Gestalt `xlat` (gestaltTranslationAttr), `_TranslationDispatch` ($ABFC) for 68k code and TranslationLib for PowerPC code: GetFileTypesThatAppCanNativelyOpen, ExtendedOpenFile, ExtendedTranslateFile, GetFileTranslationPaths, GetPathFromTranslationDialog, TranslateFile, GetDocumentKindString, GetTranslationExtensionName and the scrap calls; the Finder's open-with-translation (Apple Events from the host) and the translation choice dialog.
- **Translators:** translator extensions (Inside Macintosh: More Macintosh Toolbox, "Translation Manager") found in the System Folder's translators folder and loaded and called as Easy Open did, 68k or PowerPC; MacLinkPlus's translators installed from the user's own Mac OS 8 disk (never from the repository).
- **The host as a translator (optional):** conversions macOS can do itself (RTF, plain text, images through ImageIO) offered as translators, so classic programs open modern files.
- Apple's and DataViz's software stays out of the repository.

**Exit:** Document Converter converts a ClarisWorks and a Word 5 document to RTF with the MacLinkPlus translators; SimpleText opens a foreign text document through the Translation Manager; DataViz FileView shows a Word document.

### M17: The system libraries complete

**Goal:** Blue Box's own MathLib, StdCLib and InterfaceLib export everything Mac OS 9's did, from the published interfaces rather than one program at a time, so no PowerPC program stops on a missing import from them. Asked for 2026-10-25. CarbonLib is M20; PrivateInterfaceLib was never published and stays on demand.

- **The checklist:** `bluebox exports` reads a library's exports (an Apple stub library or the library itself) and lists those Blue Box lacks. Against Universal Interfaces 3.4's stub libraries (2026-10-25): MathLib 83 of 149, StdCLib 80 of 256 (3.7: of 312), InterfaceLib 2,196 of 2,883. Signatures from Universal Interfaces 3.4 (MPW-GM with Apple's final updates), which the user keeps outside the repository; Retro68's ImportLibraries (3.4.1) name what 3.4 lacks.
- **MathLib:** the long double routines (MathLib's long double is a double-double), classification, the environment and decimal conversions.
- **StdCLib:** MPW's C library whole: stdio with MPW's `FILE` layout (programs' `getc` and `putc` macros read it), files through the File Manager, time, locale, signals, the Pascal string routines.
- **InterfaceLib:** Mac OS 9's File Manager (FSRef, forks, iterators, catalog information, Unicode names) and its Alias and Folder Manager calls; the utilities (time conversion, atomic operations, memory, low-memory accessors, component calls, the Display Manager, Text Services, Color Picker, Dictionary Manager); old hardware and networking (SCSI, Slot, Power and Data Access Managers, AppleTalk, PPC Toolbox) answering as a Mac without that hardware or service did, with the documented errors.
- Every routine tested, as the rest.

**Exit:** `bluebox exports --missing` lists nothing for MathLib, StdCLib and InterfaceLib; a census of the PowerPC programs reports no missing import from them.

**Status (2026-10-25):** MathLib 149 of 149 (`mathlib.c`; long doubles as double-double in f1/f2, the transcendentals on the head), StdCLib 256 of 256 and 3.7's 312 of 312 (`stdclib*.c`, MPW's `FILE` and `_iob` layouts, descriptors and device switch as StdCLib 3.5 had them), InterfaceLib 2,831 of 2,883: Mac OS 9's File Manager on FSRefs (`hfsplus.c`, also `_HFSDispatch` for 68k), Driver Services, low memory, Component Manager calls, the Dictionary, Text Services and Display Managers (`driverservices.c`, `dictionary.c`, `tsm.c`, `dmgr.c`, `ilmisc.c`), and old hardware as a Mac without it, or real where Blue Box can back it: the PPC Toolbox between runs, ASP and AFP on the AppleTalk stack, file mapping (`scsi.c`, `power.c`, `ppctoolbox.c`, `xpp.c`, `fmap.c` …). Left on purpose: the File System Manager's 50 `UT*` utilities (for file system plug-ins, which manage Apple's own volume and file control blocks), NQDMisc and SndDispVersion (no published signature). Gestalt doesn't yet set gestaltHasHFSPlusAPIs ('fs  ' bit 12), as the System Blue Box reports (7.1, or 8.1 for PowerPC) lacked those calls.

### M18: Speed on the host's processor and display

**Goal:** heavy programs run well past their original Macs' speed without settings, from the CPU emulators, QuickDraw's pixel work and the display. Planned 2026-10-25, from a review of where the host's hardware could help.

- **The JIT on by default** (from M10's left items and the backlog): CR lookahead across PowerPC blocks, registers kept across blocks, translated floating point for both CPUs, the 68020's memory-indirect modes, ahead-of-time translation in `bluebox install` keyed by fragment or code resource and offset, then on by default once the corpus walk and interactive use agree with the interpreter. The x86-64 back end stays in M12.
- **QuickDraw on SIMD:** CopyBits (scaling, depth and colour conversion, the transfer modes and masks), large fills and DrawPicture's decoding (SimTower's scenery) on the host CPU's vector units: Accelerate's vImage on macOS, the compiler's NEON and SSE intrinsics elsewhere. The C paths stay as the reference, and tests compare their pixels.
- **The display on the GPU:** the step from the classic framebuffer to the screen (depth conversion, palette lookup, the window's scale such as 1.5x, the screen window) done by the GPU in the presentation layer (Metal on macOS) instead of by the emulator thread. The framebuffer stays in guest memory; only what changed goes up each frame.
- **Measuring:** `scripts/bench-jit.sh` and the game corpus of M10 (`scripts/games-m10.sh`) for the CPUs; a drawing benchmark (CopyBits and DrawPicture heavy programs headless) for QuickDraw; frame time in the windowed hosts for the display.
- **Looked at and left (2026-10-25):** the CPU emulators on the GPU (instruction execution is a serial chain of dependent steps with unpredictable memory access, the opposite of what a GPU does well), and QuickDraw drawing itself on the GPU (keeping guest memory and the GPU in step would cost more than it saves except for whole-screen blits; revisit only if a measured case calls for it).

**Exit:** the JIT is on by default with the corpus walk unchanged; the drawing benchmark and SimTower run at least twice as fast as the C paths; presenting a frame takes no measurable time on the emulator thread.

### M19: Vector and media acceleration

**Goal:** what Mac OS 9 programs did with AltiVec, vecLib, QuickTime's codecs and 3D runs on the host's own vector units, media engines and GPU. Planned 2026-10-25.

- **AltiVec:** the PowerPC G4's vector instructions (Gestalt `ppcf` and `cpuf` saying so only once they exist), interpreted first, then translated by the JIT almost one to one onto NEON (arm64) and SSE (x86-64). Programs and libraries that test for a G4 and use it (games, QuickTime's codecs, MP3 players, Photoshop's filters) take their fast paths.
- **vecLib on Accelerate:** Mac OS 9's vecLib (vDSP's FFTs and filters, vForce's array maths, vBLAS, vBigNum), a PowerPC library in M17's list, implemented on the host's Accelerate (macOS) and portable C elsewhere: big batches of work, where the host's vector units pay off.
- **QuickTime's video on the host's decoders:** codecs the host has (H.264 and MPEG-4 in QuickTime 6 era movies, JPEG and Motion JPEG) decoded by VideoToolbox and ImageIO on macOS, with Blue Box's own decoders kept as the fallback and the reference.
- **3D on Metal:** QuickDraw 3D's renderers and RAVE (`QuickDraw3DRAVELib`, Apple's 3D acceleration interface) drawn with Metal; OpenGL (today on the host's legacy OpenGL, M10) moved to Metal before macOS drops OpenGL.
- **QuickDraw 3D and RAVE design (2026-10-28):** the libraries programs import are `QuickDraw™ 3D` (about 1,200 exports), `QD3DViewerLib` (60) and `QuickDraw™ 3D Accelerator` (RAVE, 32); the archive walk's 37 programs and the PLAN's games import about 450 distinct names between them, and phase 1 aims at those.
  - *Layers.* Everything but the drawing is platform-neutral C: the object model (`q3_object.c`: objects as small system-heap handles naming host records, shared objects reference-counted, the class tree for `Q3Object_IsType` and the hierarchy calls, sets and attribute sets, groups and their positions), the maths (`q3_math.c`), geometry (`q3_geom.c`: each geometry kept as host data, its `_New`/`_GetData`/`_SetData`/`_EmptyData` converting to and from the program's structures, and broken into triangles, lines and points for drawing), views (`q3_view.c`: cameras, lights, draw contexts, renderers, styles, shaders, transforms, the submit loop in immediate and retained mode, with the state stack, transformation, lighting per vertex, clipping and projection all done on the CPU), 3DMF (`q3_file.c`: storage objects, and one reader for binary and text 3DMF, the object readers written once over a token source), the Viewer (`q3_viewer.c`) and RAVE (`rave.c`).
  - *The renderer interface* (`r3d.h`) is what a host provides: a frame (a size, a colour buffer and a depth buffer), begin with a clear colour or the destination's own pixels, state (depth test function and writes, blending as RAVE has it, a texture, its filter, wrap and operation), batches of triangles, lines and points whose vertices come already in window coordinates with 1/w, colour, a diffuse and a specular factor and texture coordinates (RAVE's `TQAVTexture`, which QuickDraw 3D's interactive renderer also reduced its drawing to), textures from 32-bit pixels, and end, which reads the frame back into host memory. The platform-neutral side then puts the frame in the window (CopyBits through the port, as AGL does, so clipping, depth and the rootless display work as for anything drawn) or in a pixmap draw context's or RAVE memory device's pixels.
  - *Hosts.* macOS implements `r3d.h` with Metal (`r3d_metal.m`): one shared-storage render target and a depth texture per frame, a pipeline per blend and texture combination, every batch of a frame encoded in one command buffer, read back at the end. Metal renders offscreen without a window server, so `swift test` and headless runs use the same path (checked 2026-10-28). Linux implements it later with OpenGL (Mesa, on Wayland or EGL offscreen); until then Linux has no `QuickDraw™ 3D` or RAVE library, as it has no OpenGLLibrary, and programs that link them weakly carry on without 3D. No software rasterizer: the CPU side's output is already what a GPU takes.
  - *Scope:* the whole documented interface, not only what the corpus calls: every routine in QD3D.h, QD3DMath, QD3DSet, QD3DGroup, QD3DGeometry, QD3DTransform, QD3DStyle, QD3DShader, QD3DLight, QD3DCamera, QD3DDrawContext, QD3DRenderer, QD3DView, QD3DPick, QD3DStorage, QD3DIO, QD3DString, QD3DExtension, QD3DController, QD3DErrors and QD3DAcceleration (QuickDraw3DLib's 1,203 exports), QD3DViewer.h (60), QD3DCustomElements.h (11) and RAVE.h with RAVESystem.h (32). `bluebox exports --missing` against the stub libraries is the checklist; the corpus checks the work and orders it. Phases:
  - *Phase 1, the path to pixels:* Q3Initialize/Exit, the object model and hierarchy, sets and attribute sets, groups of every kind, all transforms, styles, illumination and texture shaders with pixmap, mipmap and compressed textures, all three cameras, all four lights, the Macintosh and pixmap draw contexts, the interactive, wireframe and generic renderers, the submit loop in both modes with push and pop, bounding boxes and spheres, the whole maths library, every geometry's `_New`/`_Submit`/`_GetData`/`_SetData`/`_EmptyData` and per-field accessors with triangles, trimeshes, meshes (with their iterators), polygons, general polygons, polyhedra, trigrids, boxes, lines, polylines, points, markers and pixmap markers drawn, storage objects of every Mac kind, the 3DMF reader (binary and text) for every object QD3D 1.5 wrote, errors, warnings and notices; RAVE whole but colour-table textures and buffer access; the Viewer showing a 3DMF file with its controller strip and mouse orbiting. Gestalt `qd3d` and `qdrv` answer once these are in.
  - *Phase 2, the rest of the standard objects:* cones, cylinders, disks, ellipses, ellipsoids, tori, NURB curves and patches drawn as tessellated surfaces honouring the subdivision style, picking (window point, window rect and world ray picks with hit lists, pick details, the pick parts style and mesh part objects), 3DMF writing (Q3File_OpenWrite, the view's write loop, every object's writer, the primitive read and write calls), view hints, fog and anti-aliasing styles, shadow-receiving and highlight styles, the Viewer's remaining calls (cut, copy, paste, drag and drop, writing files, its state and flags), RAVE's colour-table textures and buffer access calls, Custom Elements (name, URL and wire elements).
  - *Phase 3, extending QuickDraw 3D:* classes and elements programs register (Q3XObjectHierarchy_RegisterClass, Q3XElementClass_Register and the metahandler methods, called back into PowerPC code), custom file objects and their read and write methods, renderer plug-ins a program brings (the Q3XRenderer and Q3XView calls a renderer uses), controllers, trackers and controller states (QD3DController.h, served locally since Blue Box has no 3D input devices of its own), engines a driver registers with RAVE (QARegisterEngine), the accelerator calls of QD3DAcceleration.h.
  - Quesa (LGPL) is a reference for behaviour only; none of its code is used.
  - *Status (2026-10-28, phases 2 and 3):* in: r3d.h on Metal (r3d_metal.m; r3d_none.c elsewhere), with a frame's colour and depth readable and writable mid-frame; RAVE whole (`bluebox exports --missing` 0): two engines (Blue Box's, fast, and Apple's software engine, which Combat Mission needs), z = 1 drawable (its sky), CL4 and CL8 textures and bitmaps drawn through the colour table bound to them, the draw and z buffers' access calls, textures and bitmaps made from a context; the object model, sets, strings, errors, programs' classes and elements; the maths (all of QD3DMath.h); every geometry with its accessors and tessellation (quadrics, NURBS by Cox-de Boor, NURB patches cut by their trim loops) and the mesh with holes and iterators; groups; cameras (the view-to-frustum matrix QuickDraw 3D's own, scaled by 1/yon, read from Apple's library: Bugdom's culling multiplies by w), lights, styles, transforms, shaders, pixmap, mipmap and compressed pixmap textures (a texture's alpha is transparency), draw contexts, renderers, view hints; views with rendering, bounding, picking (point, rect, ray) and writing loops (immediate-mode geometry, styles, transforms, attributes, push and pop written as objects); storage objects and the 3DMF reader and writer, binary and text, with meshes' corner and edge attribute sets and programs' registered elements and attributes (their traverse, write and read-data methods); the Viewer (the camera button's menu of views, Q3ViewerSetCustomRenderingCallback, Q3ViewerGetTransformMatrix); controllers and trackers; Custom Elements; 1.0's old names; Gestalt qd3d, q3v, q3vc; phase 3's renderer plug-ins (q3_xrender.c): a renderer class a program registers is driven through StartFrame, the default state, StartPass, state updates (matrices, attributes, styles, shaders, with or without its own push and pop), geometry whole or broken into triangles, lines, points and pixmap markers, EndPass with retraversal, EndFrame and Cancel, with its configure, nickname and configuration data methods; draw regions (Start for the destination, StartAccessToImageBuffer for a 32-bit image put into the draw context at the end of the pass) and validation flags; Q3XAttributeSet_GetPointer, Q3XView_EndFrame and IdleProgress, EiObjectHierarchy_RegisterClassByType, EiErrorClient_GetPostMethod; and the plug-ins in the Extensions folder (type 'shlb', creator 'Q3XT') loaded at the first Q3Initialize. `bluebox exports --missing`: QuickDraw3DLib 0, RAVE 0, Custom Elements 0, Viewer 0. Apple's interactive renderer from the Bugdom demo, loaded as a plug-in and made to replace Blue Box's for a try, registers and starts frames through the draw region calls and makes its RAVE contexts; it is not used (Blue Box's own is built in), and the private calls by which Q3InteractiveRenderer_ reaches into it are not documented.
  - *Settled:* backfacing style Both draws back faces lit by their own normals, as Inside Macintosh: QuickDraw 3D says (Flip is the style that turns them to face the camera), so 3D Lparser's leaves dark on one side are what QuickDraw 3D drew. NURB trim loops in 3DMF: Apple's QuickDraw 3D 1.6 had no reader, writer or text name for trim curves, so files carry none; trim loops given to Q3NURBPatch_New are drawn.
  - *Programs (2026-10-28, checked with main merged in; the base before it left some windows white after their first frames, which main had since fixed):* Bugdom's title in colour (DrawSprocket now sets the display's depth, not the back buffer's 1 bit) and its main menu drawn whole (web, cut-out icons, spider, the text), picking choosing an icon; headless, the spider turns towards it but hardly walks in the time a run gives (its speed comes from Microseconds per frame; to see in a windowed run). Gerbils! (title and fly-through with its track), Lathe, HolliDance, 3D Lparser, Quesa's Camera, Geom, Shader and Style tests, Exobattle in space and Combat Mission's battlefield draw as before; X-Plane 4.21 at 800x600 shows its own aircraft browser (not Standard File: clicks choose a folder and an aircraft), loads the aircraft and, past its no-joystick alert, starts the sim: the runway in 3D through RAVE and the instruments, though the panel's lower part still shows the splash behind it (to look at); Whurlwind 1.0d6 starts with its palette. MolView's 3DMF Viewer opens files only through Standard File, which headless runs cancel (it takes no document from the Finder).
  - *Next:* Bugdom past its menu in a windowed run; X-Plane's panel over the old splash, and flying; Bugdom asks for an ATI engine (vendor 1) and runs on with an alert without one; whether Blue Box's engine should answer as ATI's for such games is open.
- **Looked at and left (2026-10-25):** MathLib on the GPU. Apple's GPUs have no 64-bit floating point in hardware, MathLib is double and double-double precision throughout, and programs call it one value at a time, so a GPU round trip (microseconds) would cost thousands of times a host call (nanoseconds). Its batch counterpart is vecLib, above.

**Exit:** a G4-only program and an AltiVec-using one run with their vector paths; vecLib's `bluebox exports --missing` lists nothing and its FFTs match Accelerate's; an H.264 QuickTime movie plays at full frame rate through VideoToolbox; a QuickDraw 3D and a RAVE program render through Metal.

### M20: CarbonLib

**Goal:** Carbon programs written for Mac OS 8.6–9 (CFM, importing CarbonLib) run, on Blue Box's own CarbonLib built over the classic Toolbox it already has, as Apple's was. Started 2026-10-25, once M17 had completed the classic libraries under it.

- **The checklist:** `bluebox exports` against Universal Interfaces 3.4's CarbonLib stub (7,471 exports; 3,095 already Blue Box's at the start, most of Carbon being the classic Toolbox under the same names), Retro68's ImportLibraries for CarbonLib 1.3–1.4's additions.
- **Phase 1 (2026-10-25, five areas at once):** QuickDraw's port and region accessors, windows, menus, lists, TextEdit and help tags; controls (the Create…Control family and data tags) and dialogs; Carbon Events (event refs, targets, handlers, the queue, timers, RunApplicationEventLoop, the standard handlers over the classic event loop), the Multiprocessing Services on the Thread Manager, and CarbonLib loadable with Gestalt 'cbon'; Core Foundation as CarbonLib had it (CFString, collections, CFURL, CFBundle, CFPreferences, the kCF data) and Math64; the Carbon Printing Manager on the existing printing, Navigation Services on the host panels Standard File uses, the Carbon Scrap Manager, MLTE on TextEdit, ATSUI.
- **Phase 2:** QuickTime's Carbon exports (about 1,300 names in CarbonLib's list: components, codecs, movies, QuickTime VR, streaming, music), with M19's host decoders. With them, QuickTime 3 for classic programs too: Blue Box answers Gestalt 'qtim' with 2.5, and QuickTime 3's own calls (graphics importers and exporters, standard compression, the newer movie calls) are missing; Starry Night Backyard 3.03 requires QuickTime 3 and stops at its alert (2026-10-25).
- **Phase 2 progress (2026-10-28, branches qtfull and qt2):**
  - Done: every QuickTime component call exported from the interfaces' selectors and argument sizes (Generators/qt_calls.py, defs/qt_calls.tsv; 1,117 calls, floats from FPRs) through _ComponentDispatch; C components in the Component Manager with a frame for ours (qt_comp.c). QuickTimeLib missing 568 -> 14; CarbonLib missing 1,456 -> 797 (its QuickTime headers' component calls all there).
  - Done: atom containers and user data (qt_atoms.c); the Movie Toolbox's movies, tracks and media kept as records (qt_model.c) with the ~250 track, media, sample, data reference, edit, track reference, file, flattening, storage, handle and shortcut calls that used to answer zero (qt_movies.c, qt_mfile.c, qt_dref.c); time base callbacks (qt_callbacks.c); QuickTime 3-6 utilities (qt_util.c); data handlers alis/hndl/ptr/rsrc/url/null (qt_dhlr.c); media handlers vide/soun/text/sprt/musi/tmcd/moov/gnrc with the text, sprite, timecode, video, music and base calls (qt_mhlr.c); the movie controller's QuickTime 3/4 calls and actions, chapters.
  - Done (qt2): movie importers ('eat ', qt_import.c: AIFF/AIFC, WAVE with PCM, float, mu-law, A-law and both ADPCMs, System 7 sound files and 'snd ' handles, Sun audio, MIDI files as music tracks, text with QTtext's descriptors and time stamps, PICT, PICS, JPEG in place, and the images the host decodes; registered by file type and by extension) and exporters ('spit', qt_export.c: a self-contained movie, AIFF, WAVE, mu-law, System 7 sound files with the sound settings atoms, PICT, text, MIDI); NewMovieFromFile, PasteHandleIntoMovie and the Convert calls use them. Graphics exporters through ImageIO (qt_gexport.c); image codecs as 'imdc'/'imco' components (qt_codec_comp.c); the ICM's remaining calls, effects and the standard parameter dialog (qt_icm_more.c); host decoders behind hostvideo.h (VideoToolbox: H.264, HEVC, MPEG-4, H.263, DV, MPEG-1/2 where the system has them; ImageIO: Motion JPEG A, PNG, TIFF, GIF, JPEG 2000; AudioToolbox: MP3, AAC, ALAC, QDesign and QUALCOMM where the system has them), a stub with the same calls elsewhere for a GStreamer version. The Sprite Toolbox (qt_spriteworld.c); wired sprites (qt_wired.c: clicks, mouse enter/move/exit, idle and frame-loaded events; movie, track and sprite actions, variables, Case and While, expressions); tweens and the tweener components (qt_tween.c); QuickTime VR whole (qt_vr.c, qt_vr_lib.c: QTVR 1, 2 and 5 panoramas and objects drawn and navigated, hot spots, nodes, the programs' routines, the controller's VR buttons); QTStreamLib whole with RTSP/RTP playback of audio and simple video payloads (qt_stream.c, qt_rtsp.c; sending, rates other than 0 and 1 and the settings dialog answer the documented errors) and the FireWire DV handler with no devices (qt_idh.c); ShowMovieInformation's dialog and CreateMovieControl (qt_info.c). Gestalt 'qtim' and 'qtst' answer 4.1.2, 'qtvr'/'qtvv' QuickTime VR 3.0. Fixed on the way: compressed pictures' $8200 opcode was read and written without its version word (Starry Night Backyard's JPEG pictures showed only the "QuickTime and a Photo - JPEG decompressor are needed" warning; now drawn, the warning skipped once an image draws); GetMoviePict's pictures were empty (CopyBits into a colour port's own PixMap wasn't recorded). `bluebox exports --missing`: QuickTimeLib 14 (the obsolete QT 2 instrument and knob calls and MusicReceiveMIDI, whose selectors aren't in Universal Interfaces 3.4), QuickTimeVRLib 0, QTStreamLib 0, CarbonLib 547 (none QuickTime's but those 14 and the MIDI read hook UPPs).
  - Corpus (qt2): Starry Night Backyard 3.03's About crash was its own: with QuickTime 2.5 its QuickTime start-up failed and it carried on without making its sky view, then followed a null global; with 4.1.2 it starts, shows its Home Location map and its About box. Aladdin Transporter (QT 3) walks its menus. Cro-Mag Rally passes its QuickTime check and stops calling NSpInitialize, a weak import from NetSprocketLib, which Blue Box hasn't got (not QuickTime's). MoviePlayer 2.5 opens AIFF, JPEG and PNG files as movies through the importers. QTVR 1 and 2 panoramas and a QTVR 1 object from the archive display and navigate (SimpleText, QTVR controller). PictureViewer 3/4 and QuickTime Player are only inside the QuickTime 3/4 installers' compressed pieces, so not run.
  - Left: the 14 obsolete QuickTimeLib music names; tween tracks feeding modifier tracks; path, 3D and polygon tweens; wired actions on text, Flash, QTVR, music and list tracks, scripts and URLs; effects as 'eff ' components, and fire, clouds, ripple, chroma key, implode, explode, gradient and travelling matte; H.264 B-frame reordering; Sorenson, Motion JPEG B and QDesign through VLC only; a GStreamer host decoder; RTSP jitter buffer, RTCP, authentication and HTTP tunnelling; QTVR swing transitions, 1.0 hot spot names, 'navg' object-in-scene hot spots and cursors; the export settings dialogs (settings come from atom containers).
- **Later, as programs need them:** ColorSync, HTML rendering, Keychain, speech recognition, Find By Content, URL Access, Open Transport's Carbon forms.
- **Rest of CarbonLib (branch carbonrest, 2026-10-28):** done with tests: AE helpers, OSA, Translation, Internet Config, Image Capture, NSL, Dictionary, Unicode Utilities, Language Analysis, Find By Content, Speech Recognition, FontSync, Locales. Remaining: ColorSync (profile layer drafted, not in tree), HTML Rendering, the 16 headerless QuickTime Music names, and the classic stub library names in cfm.c native_libraries.
- **Test programs:** Retro68's Carbon samples, small Carbon programs built with Retro68 for each area, and the corpus's Carbon programs (Super Snake, BBEdit Lite 6.1 for OS X).

**Exit:** `bluebox exports --missing` lists only phase 2 and later names for CarbonLib; Retro68's Carbon samples, Super Snake and BBEdit Lite 6.1 run.

### M21: The Finder

**Goal:** Apple's own Finder runs in Blue Box: System 7.1's (68k) first, then Mac OS 8's and 9's (PowerPC), from the user's System disk images (Apple's software stays out of the repo). Added 2026-10-28.

- **Where it stands (2026-10-28, phase 1 largely done):** `bluebox run` and the shell's library take the Finder (`FNDR`), and the run is the system's Finder process (serial number `{0, $1001}`, type `FNDR`; events to it are its own). System 7.1's Finder shows the desktop with the disk and the Trash, opens folders (icons from the volumes' bundles through the desktop database), duplicates, renames, labels, shows Get Info and About This Macintosh, moves items to the Trash and empties it, puts away mounted volumes, launches programs as their own runs while it goes on, and quits the run on Shut Down and Restart. What it took:
  - The Process Manager's Finder-only calls (`finder.c`, `finder.yaml`), found from its code: `$14` the desktop window (outside the window list; FindWindow still answers inDesk; its visRgn is the uncovered desktop and it gets the update events for what is exposed), `$42`/`$43` volume notification, `$47` the system heap's room (with ExpandMem and its reserve), `$31`/`$32` the Apple menu's items, `$04`/`$0F` whose window is under the mouse, `$4F` a process's entry; `WakeUpProcess` ends the sleep; Quit All Applications to the system process quits the run; `MenuDispatch -5` is taken; DialogDispatch 7 and 8 (Command-period).
  - The Icon Utilities' caches, `...Method` calls, `ForEachIconDo`, mask-based regions and hit tests, labels (colours and names, `SetLabel`, tinting), and the private `$218`.
  - The File Manager: each volume's Trash and Desktop Folder at its root (`FindFolder` by volume), `PBHGetVolParms` attributes (desktop database, file IDs, CatSearch, deny modes, CopyFile, MoveRename), `PBHCopyFile`, `PBHMoveRename`, `PBExchangeFiles`, `ioACUser`, unmounting and ejecting mounted volumes, and the main disk's driver giving the drive icon.
  - The catalog (`catalog.c`, SQLite, `Catalog.sqlite` beside the Macintosh HD folder): stable directory and file IDs on the volume, folders' Finder information and comments, and each volume's desktop database (applications and icons added, built by the Finder the first time it is opened).
  - Elsewhere: zones' grow-zone functions are called (the Finder's code zone), `UnloadSeg` takes jump table entries, a headless null event moves the clock by the sleep asked, the Shutdown Manager, and events for the Finder go to a Finder run when one listens.
  - Not yet: the Finder does not save folder windows' places and reopen them (it writes no rectangle on closing or at Shut Down here: still to find why), About This Macintosh counts nothing for System Software (the memory calls answer as one flat arena), no machine icon (`micn`), labels set by the Labels control panel last only for the run, file locking (Get Info's Locked) is not kept, sleep is not offered, and the Finder learns of volumes going only by looking (the notification routine is kept, never called). A file whose Finder information says it was placed at (0,0) stays there, as on a Mac.
- **Phase 1, the Finder on its own:**
  - `bluebox run` (and the shell app) accept the Finder: file type `FNDR`, launched as the system's Finder process.
  - The Process Manager's Finder-only selectors.
  - The Desktop Database (`PBDTGetPath`, `PBDTGetIcon`/`PBDTAddIcon`, APPL mappings, comments, `PBDTGetInfo`; on the SQLite catalog, built from the volumes' `BNDL`s as the Finder's rebuild did).
  - The Desktop Folder and Trash on each volume, icon positions and window states kept in the Finder info, labels, aliases, Get Info, copying and moving through the File Manager, Put Away, Empty Trash, Eject.
  - The Finder's own trap patches, and its Apple events (the `FNDR` suite: open, print, reveal, sleep, restart, shut down).
  - Launching a program or a document starts it as its own Blue Box run, as `LaunchApplication` does today, while the Finder keeps running. The Application menu lists the other runs, through the host.
- **Phase 2, several programs in one machine:**
  - A Process Manager with real application partitions in one address space and cooperative switching at WaitNextEvent, as MultiFinder and System 7 had: layers (`_LayerDispatch` is in), suspend/resume events, the Application menu, background tasks, Apple events between processes delivered in-machine, the Finder as the process programs quit back to, desk accessories in their own layer (M-later's Device Manager has them), `LaunchApplication`'s whole interface.
  - A Blue Box run then holds a whole session: the Finder plus the programs opened from it.
  - The single-program run stays the default for launching from the host.
  - What phase 1 showed it needs: the desktop window in a layer of its own behind the applications' (now kept outside the window list), the Finder's layer walk (`LayerDispatch -1` and the layers' `$90`/`$94` links, which Shut Down uses to find the processes to quit), real process entries for `$4F` and the cursor owner calls `$04`/`$0F`, Quit All Applications delivered to each process in turn before the Finder, Apple menu items (`$31`) and the program-starting menu call (`MenuDispatch -5`) kept for a shared menu bar, the volume notification routine called on unmount, and temporary memory answering from the free memory between partitions (About This Macintosh's bars).
- **Phase 3, Mac OS 8 and 9's Finders:**
  - PowerPC, on the Appearance, Window, Menu, Control, Icon Services, Contextual Menu, Navigation and PrivateInterfaceLib work of M17/M20.
  - Finder extensions (Contextual Menu plug-ins), Sherlock's Find By Content hooks, the Finder's scripting dictionary, the Desktop printer and spring-loaded folders.
- **Approach:** as with the private libraries, the Finder's private calls are found from its code and implemented on Blue Box's real structures; Mac OS 8 and 9's CFM dumps name the PowerPC ones.

**Exit:** System 7.1's Finder shows the desktop and the volumes, opens folders, copies, moves, trashes and renames files, rebuilds its desktop database and launches programs and documents; in phase 2 several programs run beside it in one session and quit back to it; in phase 3 the Mac OS 8.1 and 9.x Finders do the same.

### M22: Classic 68k compatibility (System 6 and 7 programs)

**Goal:** programs written for System 6 and early System 7 run as they did, beyond what the shared Toolbox gives them. Most of the Toolbox has one implementation serving 68k traps and PowerPC imports, so M17/M20's work already reaches 68k programs wherever a call has a trap; what's here is what older programs depend on that Mac OS 9 had dropped or changed. Added 2026-10-28. As everywhere (Completeness, 2026-10-28), the whole of each area, with the archive walks to check it.

- **24-bit addressing:** programs in a 24-bit partition that keep flags in a pointer's high byte (the 68k-exceptions work, 2026-10-27, found these still go wrong). The CPU has `addr_mask`, but it can't simply be 24 bits: Blue Box's ROM stubs and the screen live at `$F0`/`$F8xxxxxx`. Design: 24-bit runs map those at 24-bit addresses (ROM at `$40xxxx`-style, screen inside the 16 MB space) and mask every guest access, as a Mac without 32-bit mode did; `StripAddress`, `SwapMMUMode`, `GetMMUMode`, `MMU32Bit` and the master pointers' flag byte follow the mode; the per-program setting chooses.
- **The 68000 CPU:** a true 68000 mode (odd-address errors, the 24-bit bus, `MOVE from SR` unprivileged, no 68020 addressing modes or instructions, the 68000's exception frames) and a 68020/68030 mode with their caches (self-modifying code that worked without `FlushInstructionCache`), chosen per program, alongside today's 68040 with FPU.
- **Hardware the program touches directly:**
  - The VIA (timers, the one-second interrupt, the sound and video page bits) and the SCC (serial, the AppleTalk-less polled mode) as memory-mapped devices.
  - The classic screen: `ScrnBase`, a 512×342 one-bit screen with the alternate page, and the Mac II video cards' frame buffers, so programs that write the screen directly draw.
  - The sound buffer at its fixed address and the alternate sound page, played through the host.
  - The mouse and keyboard low-memory globals programs read directly (`MTemp`, `RawMouse`, `KeyMap`).
- **The Sound Driver:** the pre-Sound Manager `.Sound` driver's square-wave, four-tone and free-form synthesizers (`StartSound`, `StopSound`, `SoundDone`, `GetSoundVol`/`SetSoundVol`), on the host's audio. Many early games use it.
- **System 6–era interfaces and behaviour:**
  - `SysEnvirons` answered as each machine did, and older ROM and machine-type checks (`ROM85`, `HWCfgFlags`, `CPUFlag`, the ROM version word) giving answers those programs accept.
  - Running without MultiFinder: the single-application `Launch`, `Chain`, `ExitToShell` to a shell; Switcher.
  - The old Standard File, Print Manager (`PrGlue` and the `.Print` driver's calls) and List Manager variants.
  - INITs and cdevs: running a program's own INIT at startup and showing a cdev in a Control Panel window.
  - Trap patching through `SetTrapAddress` with the old semantics: no `NGetTrapAddress` distinction, and tool and OS traps sharing one table on the 64K and 128K ROMs.
  - The 64K/128K ROM programs' expectations (the ROM's own routines called at fixed offsets): answered where the program would otherwise stop.
- **Copy protection and key disks:** programs that check a floppy's sectors pass when the original disk image is given (a disk image as `.Sony` drive 1 with its raw sectors).
- **Validation:** the walks' 68k stops sorted by these causes; System 6 and 7 programs from the archive and the System 6 image (`system6`) with the oracle (Basilisk II, Mini vMac for 68000 behaviour).

**Status (2026-10-28): the hardware, CPU-model and addressing halves integrated** (m22hw merged into main, m22cpu merged with main and integrated). Tests: `HardwareTests.swift`, `ClassicStartupTests.swift`, `CPUModelTests.swift`, `AddressingTests.swift`, `MachineLayoutTests.swift`.

- **One machine, one source of truth.** `--machine plus|se|classic|macii|quadra` (hw68k.c's profiles) sets the processor, FPU and addressing first (Plus, SE, Classic: a 68000, no FPU, 24-bit; Mac II: a 68020 with a 68881, 24-bit; Quadra, the default: a 68040 with its FPU, addressing by the program's `SIZE`); `--cpu`, `--fpu` and `--addressing` (and the settings) then change them, so `--machine se --cpu 68030` is an SE with a 68030 accelerator. machine.c answers everything that reports the machine: `machine_gestalt_type()` (the named machine's, else the one standing for the processor), `machine_env_type()` (SysEnvirons' machineType, Gestalt's less two), `machine_cpu_model()`/`machine_fpu_model()`, `machine_mmu_type()` and `machine_hwcfg()` (the machine's own HWCfgFlags bits, with the FPU bit and the paged-MMU bit from the processor run: the Mac II's AMU isn't one). Gestalt `mach`/`proc`/`fpu `/`mmu `/`addr`, SysEnvirons, `CPUFlag`, `MMUType`, `HWCfgFlags` and `MMU32Bit` all come from there; hw_boot no longer writes its own.
- **Memory layouts per machine** (mem.c's table, `MEM_LAYOUT_*`):
  - no machine named, 32-bit: as always (heaps from `$4000`, ROM `$F0000000`, screen `$F8000000`).
  - no machine named, 24-bit (a 68000 or a program not 32-bit clean): RAM and the partition to `$B00000`, the screen in a card's 1 MB at `$B00000` (8 bits at 1024×768; the slot space after it empty), the I/O at `$F00000` (the Quadra's chips mirrored there from `$50F0xxxx`), the ROM at `$F80000`.
  - Plus, SE, Classic (always 24-bit: their bus decoded 24 bits whatever the processor): RAM to 4 MB (`MemTop` `$400000`), the system heap the first megabyte, the 512×342 screen at `$3FA700` (`ScrnBase`, `$5900` below the top) with the alternate page `$8000` below, the sound buffer at `$3FFD00` and its alternate `$5C00` below, the partition ending under the alternate screen page (`BufPtr` `$3F2700`), the ROM at `$400000`, the chips at `$800000`–`$EFFFFF` (SCC `$9FFFF8`/`$BFFFF9`, IWM `$DFE1FF`, VIA `$EFE1FE`). `SwapMMUMode` can't leave 24-bit mode.
  - Mac II in 24-bit mode: 8 MB of RAM, the ROM at `$800000`, the screen on a video card's 1 MB of slot space at `$900000` (slot 9: 640×480 at 8 bits fits, 32 doesn't), the chips at `$F00000` and at `$50F00000` (the Mac II's 24-bit map mirrored them; a program switched to 32-bit mode by `SwapMMUMode` reaches the second). With `--addressing 32` a Mac II gets the flat layout with its chips at `$50F00000`.
- **The interpreter and the devices.** The device windows are in `mr*`/`mw*`, which both builds of the interpreter share (`cpu68k.c` and `cpu68k_strict.c`), so every model reaches them; a 68000's odd word or long faults before the device is asked, the 68020/68030 caches don't see device stores. The JIT runs only when it can see everything: not for a 68000, a 68020/68030 with its cache on, or a machine chosen by name (`g_hw_jit_off`).
- **24-bit addressing:** `--addressing 24|32` and the `addressing` setting choose; by default a 68k program whose `SIZE` resource lacks the 32-bit-clean flag (or that has none) gets 24-bit, and a 68000 always does. A 24-bit run makes the arena 256 mirrors of one 16 MB space, so the CPU's, the Toolbox's and the JIT's accesses all ignore the top byte without a mask anywhere, and lays the space out by the machine (below). `GA_ROM`, `GA_SCREEN`, `GA_A5WORLD_TOP_24`, `GA_SYSHEAP_END`, `GA_APPHEAP` and `GA_STACK_24` are variables that follow the layout (`mem_set_layout`); `mem_strip()` gives an address without its flags in 24-bit mode. The Memory Manager keeps a relocatable block's lock/purge/resource flags in its master pointer's top byte (HLock sets bit 7 there; a program's own `BSET #7,(A0)` locks the block; HGetState/HSetState are the whole byte; purging leaves NIL), and strips flags from every address it is handed. `StripAddress`, `SwapMMUMode` (switches a 24-bit machine; a 32-bit one stays), `MMU32Bit`, Gestalt `'addr'` and `Translate24To32` follow the mode. Dungeon of Doom 4.0 (its LoadSeg patch locks segments by their master pointers) runs the same in both modes. The walks' 125 distinct "heap damaged" and "exception N with no handler" stops (rerun1), run again in the default mode and in 32-bit with the heap check: the same outcome in both but where timing differed (a handful, either way): none turned on the addressing mode. Vampire Castle's was the Memory Manager's own (MaxMem's answer couldn't be had), fixed; it now stops for want of the MacinTalk driver file. Most of the rest are programs' own writes past their blocks, which the heap check catches and a Mac mostly didn't notice (to look into apart).
- **CPU models:** `--cpu 68000|68020|68030|68040` and `--fpu none|68881|68882|68040` (settings `cpu`, `fpu`; the shell's Machine section): with no machine named, a Mac SE, Mac II, Mac IIci or Quadra 800 (`machine_gestalt_type()`), with a 68882 by default on the 68030, none on the 68020 and 68000, the 68040's own on the 68040 (none: a 68LC040). The 68000: address errors for odd word/long accesses and jumps with the group 0 frame, the chip's stacked PC and register side effects (all 1,000,060 SingleStepTests 68000 vectors pass, the ~350,000 address-error cases included), `MOVE from SR` unprivileged, no format word, the 68020's instructions and modes illegal, always 24-bit. The 68020/68030: their instruction caches (the program's own stores don't reach them; FlushInstructionCache, HWPriv 0/1/9, FlushCodeCache, CACR's clear bits and the Toolbox's own stores flush), CACR as each chip keeps it, CAAR, the 68030's PMMU instructions as registers; the 68030's data cache is the CACR bits only (write-through: nothing a program sees). Format 2 frames for zero divide, CHK, TRAPV/TRAPcc and trace from the 68020 on; RTE takes each format. FPU: F-line without one; FSAVE idle frames per FPU; the FS/FD rounding forms are the 68040's. Gestalt `proc`/`fpu `/`mmu `/`mach`, SysEnvirons (from Gestalt), `CPUFlag`, `HWCfgFlags` and `MMUType` agree. The interpreter is built twice (`cpu68k_strict.c`), so the 68040 path pays only one flag test an instruction (about 4% on a tight loop). Checked on the M10 games under each model (all keep running as a 68020 and 68030, with and without an FPU; as a 68000 all but those written for a 68020: Prince of Persia's scaled index, SimTower, Star Chaos), and on the archive's old programs below (the same outcomes as a 68000 as on the 68040, but Solarian II, a Mac II game, faulting on an odd word).
- **Hardware, Sound Driver and System 6 pieces:**
  - `hw68k.c`: memory-mapped devices in the 68k interpreter (mem.h's `g_hw_map`, one byte a megabyte; the JIT doesn't reach them, so `--machine` runs without it): VIA 1 (timers 1 and 2 on the 783,360 Hz clock, free-running and one-shot, IFR/IER, the 60.15 Hz retrace and one-second flags, port A's screen/sound page and volume bits, port B's sound-off, mouse button and ADB lines, interrupts to a program's `Lvl1DT` entries or its own level 1 vector with RTE), the SCC (register-pointer protocol, polled, baud rate from the time constants, data to and from the modem and printer serial ports, DTR and break), the IWM with no drive, and VIA 2 and ASC as registers that hold what was written.
  - Machine profiles, `--machine plus|se|classic|macii|quadra` (quadra the default): the chips at the machine's addresses (the compact Macs' 24-bit I/O map `$EFE1FE`, `$9FFFF8`, `$BFFFF9`, `$DFE1FF`; `$50F0xxxx` on the II and Quadra), the built-in screen (512×342×1 for the compact ones), ROM85, HWCfgFlags, the ROM version word, SysEnvirons, Gestalt, and System 6.0.8's Gestalt (no System 7 selectors) for those that ran it.
  - The compact Macs' alternate screen page (`ScrnBase` less `$8000`, shown when VIA port A bit 6 is clear; direct writes to either page draw) and sound buffers (370 samples a retrace from the main or the alternate buffer, `$5C00` apart, played at 22.25 kHz at the VIA's volume while port B bit 7 allows it).
  - The input globals (`MTemp`, `RawMouse`, `Mouse`, `MBState`, `KeyMap`, `KeyLast`) were already kept current each slice (`events_sync_lowmem`); now tested.
  - The Sound Driver (`.Sound`): square-wave, four-tone and free-form synthesizers with completion, `StopSound` as KillIO, `SoundPtr`/`SoundActive` kept, the volume from `SdVolume`; tested on the records' real layouts.
  - `SysEnvirons` per environment version (every version fills the whole record, the AppleTalk driver and System Folder volume included, as Inside Macintosh V's version 1 did: Anthem asks for 1; 3 and up envVersTooBig as version 2, 0 envBadVers), per machine.
  - `_Chain` ($A9F3); INITs (`--init FILE`, run in ID order in the system heap before the program) and cdevs (a cdev file runs in a Control Panel host: macDev, initDev, activDev, nulDev, updateDev, hitDev, keyEvtDev, deactivDev, closeDev); the System 6.0.8 General, Keyboard and Mouse control panels run; PAT 16 (the desktop pattern) is in the System resources for General.
  - Old trap numbering in `SetTrapAddress`/`GetTrapAddress` (OS below `$50` and `$54`/`$57`, whole trap words naming their table) tested.
  - Key disks: `.Sony` (refnum -5) on a raw image given with `--floppy` (a 400K/720K/800K/1440K sector dump or Disk Copy 4.2 with its tags): block reads and writes at drive 1 and at the image's own drive-queue element, `SetTagBuffer`, drive status, format list, eject.
- **Archive check:** the walk logs hold no program stopping at a hardware address; the scan for `$EFE1FE`-style constants in the corpus' resource forks matched only data, so no archive program for these parts is known yet. System 6.0.8's own control panels (from `system6-base.img.gz`) are the real programs run.
- **cdevs as the Control Panel ran them:** the host follows the cdev's `mach` resource (softMask `$0000` and hardMask `$FFFF`: ask with macDev; otherwise every bit clear in softMask clear in ROM85 and every bit set in hardMask set in HWCfgFlags), passes CPanelID -15904 (the Control Panel's own DRVR 15 resources; the System has its PAT -15904, by which the Keyboard cdev dims its items), and stops calling a cdev that answers cdevGenErr, cdevMemErr or cdevResErr. Before, macDev went to every cdev with cdevUnset, which System 6's cdevs (mach `$FFFF0000`) take as their storage handle: harmless on a 68040, an address error on a 68000.
- **Validation after the integration (2026-10-28):** Dungeon of Doom 4.0 and the M10 games in ten configurations (default, `--addressing 32`, `--cpu 68000/68020/68030`, `--cpu 68020 --fpu 68881`, `--machine plus/se/macii`, `--machine plus --cpu 68020`): Dungeon of Doom, Glider+, Shufflepuck, Spectre, Bolo, Crystal Quest and Snood run in all. Prince of Persia runs as a Plus and an SE (it takes its black-and-white path there) but not as a bare `--cpu 68000` (a 68020 scaled index on its colour path, as before). Star Chaos faults as a 68000 on a Plus or SE as with `--cpu 68000` (written for a 68020), and on a Mac II asks for QuickTime, which System 6 didn't have; SimTower asks for System 7 on every named machine; Solarian II quits on a Plus or SE (a Mac II game wanting Color QuickDraw) and runs on the Mac II. The archive's 19 old programs (the 68000 check above) as a Plus and an SE: Blind Watchmaker, LAZlife, Star Flight and Lunar Rescue now get through the menu walk as a Plus (on the default machine they stop: A-line traps they don't expect, writes past their blocks on the big screen); Monopoly 4.0 and 4.03a and Klondike stop at different heap damage (their own writes past blocks); Vampire Castle jumps through a nil handle where its MacinTalk driver should be (the file it needs is missing, as before); the rest as on the default machine. System 6.0.8's General, Keyboard and Mouse cdevs (from `system6-base.img.gz`) run as a Plus, an SE and the default machine.
- **Not done:** the 24-bit heap's own block header format (8-byte headers, size in the low 24 bits) for programs that walk the heap themselves (heaps keep the 32-bit format); the JIT for the 68000 and the cached models; an odd PC on the 68020 and later; CALLM/RTM and the 68851; a Mac II video card's own registers (its frame buffer is the screen in slot 9, plain memory); the IWM's GCR timing; SCC interrupts and the VIA's shift register (the Plus keyboard protocol); mouse quadrature; Switcher; the Sound control panel (Sound Manager 1's `_SoundDispatch`); `PrCtlCall` and the `.Print` driver's own calls; the old Standard File and List Manager variants rechecked against System 6 programs.

**Exit:** System 6–era programs from the archive that write the screen or sound directly, use the Sound Driver, or need 24-bit addressing or a 68000 run as on their machines; a program's INIT and cdev run; a key-disk game passes with its disk image.

### Backlog

Work found along the way that belongs to no milestone yet (2026-10-22):

- **JIT** and **QuickDraw speed:** moved to M18 (2026-10-25).
- **Redraw check:** about 200 programs redraw differently from their first drawing (`--redraw-check`, 2026-10-16), many as a Mac did; to be judged against the oracle.
- **Converters and international resources:** a program's own Unicode fallback handler, stateful encodings (ISO-2022, HZ, UTF-7) across calls, a better sniffer, CountUnicodeMappings and the TEC plug-in interface; itlm, itl5, IUDateString with itl1's abbreviations, StyledLineBreak on the line-break table, IntlTokenize; and the loop in Apple's own Text Encoding Converter (its INIT's slot in the international globals).
- **OpenGL:** colour-index mode, accumulation buffers, the imaging subset, GLU's tessellators and NURBS, aglSetFullScreen changing the display's resolution.
- **Programs:** sixtyforce's Video Settings dialog draws empty (its filter takes the update event with BeginUpdate and EndUpdate for a dialog it keeps in a local; whether Mac OS 8's Dialog Manager drew the items anyway needs a PowerPC oracle); the Warlock demos' WindowHider WDEF reads a CGrafPort's grafVars as portBits.bounds (find on the oracle why that works); StarGuardian's AddColor XCMD fails on pathnames of 128 characters or more (granted folders shown nearer the root would avoid it); Internet Explorer 4.01's drawing into a disposed port (now harmless).
- **AppleShare volumes kept current:** the mirror (`appleshare.c`) lists the server's tree once when the session opens and each folder at most once after, so files others add, delete or rename on the server after that don't show until the session ends (asked 2026-10-25). Two complementary ways: (1) re-list a folder from the server when a program enumerates it (GetCatInfo by index, the host Open panel) and its listing is older than a few seconds, merging the difference into the cache folder (new placeholders, removed entries, renames by AFP file ID, changed sizes, dates and Finder information) without disturbing open forks or the program's own pending changes; (2) the server's own notices: AFP 2.1's attention codes and AFP 3's server notifications (FPZzz, the volume-modified attention) and a volume's modification date (FPGetVolParms) checked at the same time, so an unchanged volume costs one call. A file another client changed while the program has it open stays as the program read it, as on a Mac, until it is closed and reopened.
- **Subsystems the full archive walk found missing (2026-10-25, first 19 batches of the Vintage Apple Macintosh archive; counts are programs blocked):** QuickDraw 3D and RAVE 22 (X-Plane 4.21's suite, Combat Mission, Bugdom through FragPrepare on its own QD3D libraries, Exobattle, 3D Lparser; none checks Gestalt), QuickTime's Sequence Grabber 11 (BTV Edit/View, VideoIN, BigEye; they would want a video digitizer next), NetSprocket 7 (AstroRock, Psion Boy, Unprovoked), QuickTime 4 (Cro-Mag Rally asks for 4.1.2; the graphics importers are in since 2026-10-25), CFM-68K (Flip-O-Matic), AutoDoubler's decompression vector at $7F0 (5 of a Kind), mounting a volume through a program's own disk driver (AppDisk, ShrinkWrap images).
- **Open Transport STREAMS:** Blue Box's Open Transport (M9) is endpoints on host sockets, without OT's STREAMS layer (stream heads, modules, `OTStreamOpen`/`OTStreamOpenPriv`, `OTIoctl` on a raw stream, DLPI). The archive walk's rerun (2026-10-27) found 30 programs stopping there, all network tools that work on OT's internals: IPNetRouter (14; it pushes its own NAT module onto the IP stream), IPNetMonitor and its SetupAP (10), OTSessionWatcher, OTAdvancedTuner, StreamWatcher. Ordinary programs never need it. Doing it would mean a STREAMS framework with the ip/tcp/udp modules on host sockets and an interface to the host's packets (privileges, a tun/utun device); low priority.
- **Native dialogs:** alerts are shown as native host alerts, but a program's own modal dialogs are not; one made only of buttons, static text, icons and pictures (no user items, edit text or controls of its own, and a filter that draws nothing) could be shown natively too, its button the item ModalDialog returns. Apeiron's PowerPC registration dialog (DLOG 900) would not qualify: its text is drawn into user items, where the 68k build's ALRT 510 has static text.
- **Deferred on purpose:** CFM-68K (68k programs in PEF containers; IE 4.01's 68k build). CarbonLib is M20 (2026-10-25).

## 6. Risks

| Risk | Mitigation |
|---|---|
| Long tail of undocumented behaviour | Oracle probes and trace diffing from M0, a quirks database, and the trap census to focus effort |
| Apple deprecates or removes Carbon `SpeechSynthesis` | `avspeech` backend with automatic fallback; the `record` backend keeps the API behaviour tested |
| Linux speech quality and phoneme fidelity | Use speech-dispatcher's pluggable synthesizers; document which features are approximated; neural voices through Piper |
| Rootless limitations on Wayland | Per-window coordinates, interactive move and resize, per-window menus; screen mode as the universal fallback |
| Swift on Linux and GTK interop | A thin hand-written C wrapper, no third-party bindings; CI on Linux from M12 |
| Interpreter speed | Pre-decoded caches in M7; JIT and translation cache in M10 (`--jit`, 68k and PowerPC on arm64, 5–12x on busy code) |
| Legal | Clean-room implementation, MIT licence, no Apple ROM/System/fonts shipped, the oracle kept separate and dependent on user-supplied files, imports only from user-provided files and URLs |
| Classic codecs and AppleTalk interoperability | Our own decoders for the classic QuickTime codecs; AppleTalk link layer tested against another emulator and a router before M5 exits |
| Scope creep | Each milestone is gated on named apps and exit criteria; anything new goes into a later milestone |

## 7. References

- Executor (ARDI, MIT) and its maintained fork: Toolbox and Memory Manager implementation reference.
- syn68k (MIT), Musashi, Moira: 68k core references.
- Retro68: cross-compiler for probe apps.
- Basilisk II / SheepShaver (GPL): oracle only.
- Inside Macintosh (all volumes), Speech Manager docs, Technical Notes, MPW Universal Interfaces: API definitions and struct layouts.
- SingleStepTests 680x0: CPU test vectors.
