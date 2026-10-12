# Classic: a Mac Toolbox environment for modern macOS and Linux

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
| Binaries | Run existing 68k and PPC binaries unmodified. No source toolchain for classic apps. |
| Process model | One classic app per host process, launched from the host OS. No inter-app communication except via host clipboard and files. No Finder. |
| Core language | C17 for the CPU cores, memory model, trap dispatch, and the whole Toolbox. |
| Host language | Swift for the macOS host and the CLI. Python for code generation (and possibly import/install tooling). |
| Linux UI | GTK4/GDK. Open: written in plain C (GTK is a C library) or Swift through a module map; decide before M2. |
| macOS UI | AppKit, CoreText, CoreAudio. |
| Build | One SwiftPM package (C targets + Swift targets). |
| License | MIT. GPL code may be used only in the separate, never-linked test oracle. |
| ROM / System | Not required, never shipped. We ship our own compatibility System resource file. Anything the ROM provided is reimplemented natively or authored by us. |
| File storage | AppleDouble (`._` sidecar) is the only fork format we write. Native xattr forks are read only at import/heal time. |
| Metadata | SQLite catalog: directory IDs/CNIDs, Finder info, name mapping, resource index, font IDs, app profiles. Resource *data* stays in real resource forks. |
| Disk images | Read-only HFS (later HFS+) image support, for import and for mounting as volumes. |
| Memory | A flat 4 GB guest arena, reserved but committed lazily. No per-app memory partitions. |
| Windows | Rootless (one host window per Mac window) by default. Screen-in-a-window chosen automatically when an app needs it. |
| Speech | **MacinTalk / Speech Manager support is required.** It has three backends: Carbon `SpeechSynthesis` and `AVSpeechSynthesizer` on macOS, speech-dispatcher on Linux. |
| Extensions | Classic INITs are not loaded. Their functionality is reimplemented as built-in modules: speech, printing, networking and so on. |
| 64-bit | Existing binaries stay 32-bit. New software can be written against a native 64-bit SDK built on the same Toolbox core (M9). |

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
- **`SIZE` resource:** only its flags are honoured. Apps that are not 32-bit clean run in 24-bit mode, with their heap and stack placed below 16 MB.
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
- **Native SDK (M9):** the same functions behind opaque types and accessors, in host byte order, with 64-bit pointers.

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
- **Remembering the mode:** the decision is stored in the app's profile, and a manual override is available.
- **Clipboard:** the Scrap Manager syncs with the host pasteboard (`TEXT` ↔ UTF-8, `PICT` ↔ PNG/PDF).
- **Standard File, Navigation Services and the print dialogs** use native host dialogs when the app installs no hooks, and a classic-style dialog when it does.

### 2.5 Fonts

- **System fonts:** the compatibility System file ships bitmap clones of Chicago, Geneva, Monaco and New York (9/10/12 pt and others) with the original metrics, so layouts match.
- **Host fonts:** a virtual "Host Fonts" resource file in the resource chain creates `FOND`, `NFNT` and `sfnt` resources lazily, only when something asks.
  - Rendering goes through CoreText or FreeType.
  - Characters map from MacRoman through each font's Unicode `cmap`.
  - Family IDs are stable, stored in the catalog, and kept clear of the classic ID numbers.
  - An allow-list keeps Font menus usable.
  - Classic names map to host equivalents: Times, Helvetica, Courier, Symbol.
- **Classic font files:** suitcases (`FFIL`) containing bitmap (`FONT`/`NFNT`) and TrueType (`sfnt`) fonts, from a host fonts folder that appears as the System Folder's Fonts folder. Classic TrueType renders through the same host rasterizer as host fonts.
- **No PostScript Type 1 / ATM:** `LWFN` outline fonts are out of scope. Documents that use them fall back to the matching bitmap `NFNT` or a host-font substitute.
- **Smoothing:** 1-bit rendering by default for exact output. Grayscale smoothing is an option.

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
   classic run --gestalt sysv=0x0755 --lowmem DoubleTime=20 --speech=avspeech <app> [docs…]
   ```

Gestalt selectors can be overridden (`sysv`, `mach`, `cpu `, `fpu `, `qd  `, `sysa`, feature bits). Low-memory globals can be overridden only if they are on the whitelist of real settings. Derived state such as `ApplZone` or `CurrentA5` is never settable. The shell can push changes while an app runs, for example when the host's double-click time changes.

### 2.7 Built-in extension modules

| Module | macOS | Linux | Milestone |
|---|---|---|---|
| **Speech Manager / MacinTalk (required)** | `SpeechSynthesis` (default), `AVSpeechSynthesizer` | speech-dispatcher (`libspeechd`) | M3 |
| Printing Manager → PDF / native print | CoreGraphics PDF, `NSPrintOperation` | cairo PDF, GtkPrint/CUPS | M3 |
| Sound Manager | CoreAudio | PipeWire | M5 |
| Drag Manager ↔ host drag and drop | AppKit | GTK4 | M7 |
| MacTCP / Open Transport → sockets | BSD sockets | BSD sockets | M7 |
| QuickTime (playback first) | AVFoundation | GStreamer | M8+ |
| Sound input, InputSprocket, OSA, QD3D | — | — | Later |

### 2.8 Launching from the host

- `classic run <app> [docs…]` is the direct and development entry point.
- `classic install <app>` wraps a classic app as a host app.
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
│  ├─ CPU68K/           C: 68020 interpreter
│  ├─ CPUPPC/           C: PPC interpreter (M6)
│  ├─ Toolbox/          C: trap dispatch and the managers (Memory, Resource, Segment Loader,
│  │                    Gestalt, ...); include/ holds the internal headers and the public machine.h
│  ├─ Modules/          C: Speech, Print, Sound, Net, ...
│  ├─ ForkStore/        C: AppleDouble, MacBinary, resource fork parsing and 'dcmp' decompression;
│  │                    later HFS/HFS+ images and xattr import
│  ├─ Catalog/          C: SQLite catalog
│  ├─ HostAPI/          C header: struct host_ops
│  ├─ HostMac/          Swift: AppKit, CoreText, CoreAudio, SpeechSynthesis, AVFoundation
│  ├─ HostGTK/          GTK4/GDK, FreeType, PipeWire, speech-dispatcher, cairo
│  │                    (Swift or plain C: decide before M2)
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
- `classic run <app>` in headless mode with a trap trace log.
- Oracle setup: Basilisk II fork with trap tracing, the probe app build (Retro68), the diff tool, and the trap census for TeachText, SimpleText (68k), ResEdit and HyperCard 2.1.

**Exit:** TeachText's code runs through its init sequence in headless mode, and the trace shows every trap it calls; the unimplemented ones are flagged. The memory and resource probes match the oracle.

**Status (2026-10-12):** done except the oracle.
- TeachText (System 7.1) launches: segments load through the jump table (including `dcmp` 0 compressed `CODE`), MPW's `%A5Init` runs, and it stops at `InitGraf` after 2,561 instructions. In census mode it reaches 4,817 instructions and lists 33 traps (10 implemented, 23 missing: the M1 work list).
- 68k interpreter passes all 1,000,060 SingleStepTests 68000 vectors (address-error cases, a documented `ASR` divergence and three bad vectors skipped).
- Decompression matches rsrcfork on all 601 compressed resources in System 7.1's TeachText, Finder and System.
- 38 tests pass on macOS and Linux (Swift 6.2 container), and under ASan/UBSan.
- Not yet: the oracle (Basilisk II trace fork, Retro68 probes, diff tool, real-system census), 24-bit mode, `InitZone`, grow-zone procedures, a real disassembler.

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
- **Launching:** Apple Events for `oapp`/`odoc`/`quit`, and `classic install` producing a macOS `.app` bundle with icon and document types.
- **Compatibility System file:** the first build of the resource file (defprocs, cursors, `KCHR`, standard strings and alerts, fonts).

**Exit:** TeachText is installed as a macOS app. Double-clicking a `.txt` file opens it in rootless windows; editing, copy/paste with host apps, Save As and quitting with the unsaved-changes prompt all work. All probes for the managers in this milestone match the oracle.

### M2: Linux parity and the import pipeline

**Goal:** everything from M1 works on Linux, and classic software can be brought in from archives and disk images.

- **Linux host (HostGTK):**
  - GTK4 rootless windows with per-window Mac coordinates
  - moves and resizes through `gdk_toplevel_begin_move`/`gdk_toplevel_begin_resize`
  - menu bar in each window, `xdg_popup` menus
  - pasteboard, native file dialogs
- **Linux launching:** `classic install` on Linux produces a `.desktop` file, a PNG icon and shared-mime-info XML.
- **HFS images:** read-only reader for HFS images (DiskCopy 4.2, raw, NDIF/UDIF with ADC, hybrid ISO), which can also be mounted as volumes.
- **`classic import <file|url>`:**
  - archives (`.sit`, `.sea`, `.hqx`, `.bin`, `.cpt`) unpacked with `unar`, keeping forks
  - disk images through our HFS reader
  - everything written to AppleDouble and catalogued, with applications detected (`APPL`, 68k vs PPC)
  - works on files and URLs the user supplies; no crawler
- **`classic heal`:** converts xattr-only resource forks to AppleDouble.
- **CI:** Linux CI, headless probe runs on both platforms.

**Exit:** a TeachText `.sit` archive downloaded from Macintosh Garden imports, installs and runs on macOS, and on Linux under both Wayland and X11. HFS floppy images mount read-only.

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
- **Callbacks:** text-done, speech-done, sync, error, phoneme and word callbacks. These are delivered at safe points, honouring the A5 world, and through Mixed Mode in M6.
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

### M4: ResEdit and screen-mode detection

**Goal:** ResEdit 2.1.3 runs well enough to inspect and edit real applications. This stress-tests the Resource Manager.

- **Resource Manager write path:** `AddResource`, `ChangedResource`, `RmveResource`, `UpdateResFile`, `WriteResource`, `SetResAttrs`, `SetResInfo`, map compaction, and creating resource forks on files that lack them. Always written atomically to AppleDouble.
- **More managers:** List Manager with custom `LDEF`s, custom `MDEF`/`CDEF` support, pop-up menus (`CDEF 63`), Color Picker and offscreen GWorlds.
- **Color QuickDraw:** complete enough for ResEdit's icon, `cicn`, `ppat` and `clut` editors.
- **Raw fork access:** `OpenRF` with `FSRead` on raw fork bytes, which must be byte-exact.
- **Automatic screen-in-a-window detection** (§2.4): framebuffer `mprotect` and the fault handler, the trigger heuristics, the runtime switch, and saving the decision to the profile.
- **Profile system complete:** the layered profile, CLI overrides, the Gestalt and low-mem whitelist, and values derived from the host.
- **`classic inspect`:** a host-side resource browser with export (`PICT`/icons → PNG, `snd` → WAV, `STR#`/`TEXT` → UTF-8).

**Exit:** ResEdit opens a copy of TeachText, edits a `STR#`, a `DLOG` and an icon, saves, and the modified TeachText runs. Fuzzed resource maps never crash the runtime. A test app that writes directly to the screen switches to screen mode on its own.

### M5: HyperCard 2.1

**Goal:** HyperCard 2.1 runs the Home stack and typical stacks, including sound, XCMDs and printing.

- **Sound Manager:** `snd ` resource playback (formats 1 and 2, sampled sound, the basic synthesizers), `SndPlay`, channels, `SndDoCommand`/`SndDoImmediate`, and callbacks. Output through CoreAudio and PipeWire.
- **XCMD/XFCN:** the callbacks themselves are HyperCard's own code, but loading the code resources, the A5 world and the calling conventions are ours to get right.
- **Heavier QuickDraw:** all `CopyBits` modes with masks, `CopyMask`/`CopyDeepMask`, `SeedFill`/`CalcMask`, and pattern and region performance.
- **Fonts:**
  - host fonts as system fonts (the virtual resource file, stable IDs, allow-list)
  - classic font suitcases (bitmap and TrueType) from the host fonts folder
- **Other managers:** Script Manager and International Utilities basics, the Help Manager (balloons), and Notification Manager basics.
- **Printing at scale:** printing cards, reports and scripts.
- **Performance:** an interpreter profiling pass and pre-decoded instruction caches.

**Exit:** HyperCard 2.1 runs its Home stack and a corpus of ten-plus stacks from Macintosh Garden: navigation, painting, sound, an XCMD-based stack, printing cards to PDF, and speech through a stack XCMD or HyperTalk `speak` where available.

### M6: PowerPC

**Goal:** PowerPC-native applications run, and fat binaries prefer their PPC code.

- **PPC interpreter:** 32-bit user mode (603/604/G3 integer and FPU), validated against vectors generated by the oracle.
- **PEF loader and Code Fragment Manager:** `cfrg`, data fork fragments, imports and exports, init/term routines, weak imports.
- **System libraries:** InterfaceLib, MathLib, ObjectSupportLib and the PPC side of every Toolbox call implemented so far.
- **Mixed Mode:** `NewRoutineDescriptor`, `CallUniversalProc`, and switching between 68k, PPC and native code in all directions, including callbacks and custom defprocs written in either ISA.
- **Speech callbacks** through Mixed Mode for PPC apps.

**Exit:** SimpleText PPC and HyperCard 2.4 (PPC) reach parity with their 68k runs from M3 and M5. A fat app picks PPC by default; the profile can force 68k.

### M7: Mac OS 8/9 surface and connectivity

**Goal:** a broad range of apps from the 7.5–9.2 era runs.

- **8/9-era managers:**
  - Appearance Manager (themes: Platinum by default, System 7 look optional)
  - Navigation Services (native dialogs when no hooks)
  - Thread Manager, Contextual Menu Manager, Window Manager 2.0 additions, Control Manager 2.0
- **Drag Manager ↔ host drag and drop:** files and text in and out of classic apps.
- **Open Transport and MacTCP** on host sockets: TCP, UDP and DNS.
- **Apple Events:** a broader set, plus a host-side "do script" hook.
- **Corpus expansion:** office, graphics and internet apps, with a quirks database.

**Exit:** a defined 8/9 corpus (to be chosen at M6) passes its smoke tests, and a classic FTP or IRC client connects to a real server.

### M8: Performance and media

**Goal:** heavy apps and games run at a usable speed, and QuickTime playback works.

- **JIT** for 68k and PPC on macOS (`MAP_JIT`) and Linux.
- **Persistent translation cache** keyed by a hash of each code resource or fragment, with an ahead-of-time "pre-translate" option in `classic install`. The interpreter remains the fallback.
- **QuickTime:** movie playback (`NewMovieFromFile`, a basic Movie Toolbox, the standard controller) on AVFoundation and GStreamer.
- **Games:** InputSprocket → host gamepads, DrawSprocket → screen mode, and Sound input.

**Exit:** a defined game and media corpus runs at full original speed or better.

### M9: Native 64-bit SDK

**Goal:** new software can be written against the classic Toolbox as native 64-bit host code.

- A C header and library exposing the Toolbox through opaque types and accessors (Carbon-style), in host byte order with 64-bit pointers. `NewPtr` and `NewHandle` are backed by host `malloc`.
- Toolbox-owned objects stay in the guest arena behind opaque references. Bulk data is passed by host pointer.
- Generated from the same definitions as the 68k and PPC glue.
- Swift and C sample apps. Native apps use the same install path (`.app` bundle and `.desktop` file).
- Later option: WebAssembly (64-bit memory) as a portable, sandboxed format calling the Toolbox through imports.

**Exit:** a native sample app (a text editor and a drawing app) uses the classic UI, speech and printing, and allocates more than 4 GB.

---

## 6. Risks

| Risk | Mitigation |
|---|---|
| Long tail of undocumented behaviour | Oracle probes and trace diffing from M0, a quirks database, and the trap census to focus effort |
| Apple deprecates or removes Carbon `SpeechSynthesis` | `avspeech` backend with automatic fallback; the `record` backend keeps the API behaviour tested |
| Linux speech quality and phoneme fidelity | Use speech-dispatcher's pluggable synthesizers; document which features are approximated; neural voices through Piper |
| Rootless limitations on Wayland | Per-window coordinates, interactive move and resize, per-window menus; screen mode as the universal fallback |
| Swift on Linux and GTK interop | A thin hand-written C wrapper, no third-party bindings; CI on Linux from M2 |
| Interpreter speed | Pre-decoded caches in M5, JIT and translation cache in M8 |
| Legal | Clean-room implementation, MIT licence, no Apple ROM/System/fonts shipped, the oracle kept separate and dependent on user-supplied files, imports only from user-provided files and URLs |
| Scope creep | Each milestone is gated on named apps and exit criteria; anything new goes into a later milestone |

## 7. References

- Executor (ARDI, MIT) and its maintained fork: Toolbox and Memory Manager implementation reference.
- syn68k (MIT), Musashi, Moira: 68k core references.
- Retro68: cross-compiler for probe apps.
- Basilisk II / SheepShaver (GPL): oracle only.
- Inside Macintosh (all volumes), Speech Manager docs, Technical Notes, MPW Universal Interfaces: API definitions and struct layouts.
- SingleStepTests 680x0: CPU test vectors.
