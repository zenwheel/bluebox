# Blue Box

Blue Box runs classic Macintosh applications (System 7.1 to Mac OS 9.2) on
modern macOS. It doesn't emulate a whole Mac and needs no Apple ROM or System
file. Instead it reimplements the Mac Toolbox (QuickDraw, the Window, Menu,
Dialog, Resource, File and Sound Managers, and the rest) in C, and runs each
application's code on built-in 68k and PowerPC interpreters. Each classic
application runs as its own macOS application, with its windows on the macOS
desktop and its files kept as ordinary files with AppleDouble sidecars
(`._name`).

It is a work in progress. [PLAN.md](PLAN.md) has the milestones and
[docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) has the latest results from
running a corpus of applications. Linux support is planned (M12) but not
there yet.

## Building

You need:

- macOS 13 or later
- Xcode 16 or later (Swift 6 and the macOS SDK), or the matching Command Line Tools

Build:

```sh
swift build -c release
```

The program is `.build/release/bluebox`. To run the tests:

```sh
swift test
```

To start over, `swift package clean` removes the build products and
`swift package reset` removes all of `.build`. The generated sources
(`*_gen.c`) are checked in; `uv run --with pyyaml python Generators/gen.py`
regenerates them from `Defs/`.

These are optional and found at run time:

- **[The Unarchiver](https://theunarchiver.com)'s `unar`** (`brew install unar`) imports StuffIt, Compact Pro and other archives. MacBinary, BinHex and disk images need nothing extra.
- **[VLC](https://www.videolan.org/vlc/)** in `/Applications` plays QuickTime movies.

## Installing

Put `bluebox` somewhere stable on your `PATH`. The application bundles it
makes refer to it by its path, so don't move it afterwards.

```sh
sudo install -m 755 .build/release/bluebox /usr/local/bin/bluebox
```

### As a Mac application

```sh
sh scripts/make-app.sh          # makes build/Blue Box.app
```

`Blue Box.app` holds everything it needs:

- the program, with its fonts built in;
- The Unarchiver's `unar`, when it's installed here (Homebrew), so archives import without it;
- its icon (drawn by `bluebox make-iconset`) and the licences.

Opening it shows the shell. Dropping archives or disk images on its icon
imports them. Copy it to `/Applications` before using `install`, because the
application bundles that `install` makes refer to it where it is. QuickTime
movies still need VLC.app installed.

The command-line tool is the app's own program. Link it onto your `PATH` to
use it from Terminal (with no arguments it opens the shell):

```sh
sudo ln -s "/Applications/Blue Box.app/Contents/MacOS/bluebox" /usr/local/bin/bluebox
```

## Running classic applications

Classic software usually comes as an archive or a disk image. Import it into
the library (`~/Library/Application Support/Blue Box/Library`, or set
`BLUEBOX_LIBRARY` to use another folder):

```sh
bluebox import "SimpleText.sit"     # .sit .sea .cpt .hqx .bin .img .dsk .toast .iso ...
bluebox apps                        # list what's in the library
```

Then run an application by name or path:

```sh
bluebox run SimpleText
bluebox run "Prince of Persia" --depth 8
```

`run` also accepts an archive directly; it imports it first.

To make a macOS application for it in `~/Applications/Blue Box`, with its
Finder icon and with document drag and drop:

```sh
bluebox install SimpleText
```

The shell is a window listing the library's applications with their
settings. You can drop archives on it to import them:

```sh
bluebox shell
```

Some useful options for `run`:

| Option | |
|---|---|
| `--depth 1\|2\|4\|8\|16\|32` | Screen depth. Older games often want 8. |
| `--scale 1\|1.5\|2` | Make classic windows bigger on modern displays. |
| `--ppc` / `--68k` | Choose the code to run in a fat application. PowerPC-only applications always run PowerPC code. |
| `--appearance native` | Use macOS window frames. A Kaleidoscope scheme can be given instead. |
| `--headless --screenshot out.png` | Run without a window and save the screen at the end. |

`bluebox set <app> depth 8` (or `cpu`, `scale`) remembers a setting for one
application. `bluebox --help` lists everything.

## What's in the repository

| | |
|---|---|
| `Sources/Toolbox` | The Toolbox managers, trap dispatch, the Code Fragment Manager and Mixed Mode (C) |
| `Sources/CPU68K`, `Sources/CPUPPC` | The 68k and PowerPC interpreters |
| `Sources/ForkStore` | AppleDouble, MacBinary, resource forks, disk images |
| `Sources/BlueBoxHost`, `Sources/bluebox` | The macOS host and the command-line tool (Swift) |
| `Generators/` | Generates trap tables, glue and structure layouts from the API definitions in `Defs/` |
| `Tests/` | Unit tests, CPU test vectors, probe programs |

Apple software (ROMs, System files) and third-party applications are not part
of the repository. Put applications you want to try in `Software/`, which
git ignores.

## Licence

MIT, except `Sources/MACE` (LGPL 2.1 or later) and the fonts and definitions
listed in [LICENSE](LICENSE).
