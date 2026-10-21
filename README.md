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

Or with make: `make` builds `bluebox` and `build/Blue Box.app`, `make test`
runs the tests and `make clean` removes the build products and the app.

The generated sources (`*_gen.c`) are checked in;
`uv run --with pyyaml python Generators/gen.py` regenerates them from `Defs/`
and `Generators/defs/`.

### Cleaning

```sh
swift package clean      # removes the build products in .build, keeping the package cache
swift package reset      # removes all of .build: the next build starts from nothing
rm -rf build             # removes build/Blue Box.app (scripts/make-app.sh makes it again)
```

A build that behaves oddly after switching branches or Xcode versions
usually needs only `swift package clean`; `swift package reset` is for when
that isn't enough.

These are optional and found at run time:

- **[The Unarchiver](https://theunarchiver.com)'s `unar`** (`brew install unar`) imports StuffIt, Compact Pro and other archives. MacBinary, BinHex and disk images need nothing extra.
- **[VLC](https://www.videolan.org/vlc/)** in `/Applications` plays the QuickTime movies Blue Box's own decoders don't (Sorenson Video, QDesign Music and the like); Cinepak, Animation, Graphics, Apple Video, Photo-JPEG and their sounds need nothing extra.

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
| `--ppc` / `--68k` | Choose the code to run in a fat application: PowerPC by default, or 68k (as `bluebox set <app> cpu 68040` does for one application). PowerPC-only applications always run PowerPC code. |
| `--appearance native` | Use macOS window frames. A Kaleidoscope scheme can be given instead. |
| `--headless --screenshot out.png` | Run without a window and save the screen at the end. |

`bluebox set <app> depth 8` (or `cpu`, `scale`) remembers a setting for one
application. `bluebox --help` lists everything.

### Apple Events

A running application takes Apple Events as it would from the Finder or
another program. Documents opened on its Dock icon or bundle arrive as
Open Documents; `bluebox send` sends one from the command line (to the only
application running, or the one `--to` names by process ID or creator):

```sh
bluebox send odoc letter.txt
bluebox send dosc "some script"     # Do Script, for applications that take scripts
bluebox send quit
```

Their bundles are scriptable too, so AppleScript can do the same:
`tell application "SimpleText" to do script "..."`, `open`, `print` and
`quit`. Running applications also reach each other's Apple Events.

### Networks, file servers and serial ports

These can also be set per application, or for every application, in the
shell's settings window or with `bluebox set` (`appletalk`, `modem-port`,
`printer-port`, `appleshare`).

| Option | |
|---|---|
| `--appletalk local` | AppleTalk between copies of Blue Box on this Mac (the default). |
| `--appletalk lan` or `lan:en0` | AppleTalk on the local network as LocalTalk over UDP, the way other emulators and LToUDP routers speak it. Works over Wi-Fi. |
| `--appletalk bridge:en5` | EtherTalk on a wired interface, with real Macs, netatalk servers and AppleTalk routers. It asks for an administrator password when AppleTalk opens. Wi-Fi isn't offered: AppleTalk doesn't get through it. A VM's network (UTM's `bridge100`) works. |
| `--appletalk off` | No AppleTalk. |
| `--appleshare tcp:host/Volume` | Mount a file server's volume as a disk while the program runs (AFP over TCP, e.g. netatalk or Mac OS 9 file sharing). |
| `--appleshare atalk:Server:AFPServer@*/Volume` | The same over AppleTalk (with `--appletalk` on). Put `user:password@` before the server to log in; without it you are a guest. Repeatable. |
| `--modem-port S`, `--printer-port S` | What a serial port connects to: `modem` (the modem port's default: a Hayes modem whose `ATDT host:port` dials over TCP), `modem:PORT` (one that also answers calls to PORT), `tcp:HOST:PORT`, `listen:PORT`, `pty`, a device such as `/dev/cu.usbserial`, or `none` (the printer port's default). |
| `--realtime` | Headless runs follow real time, so networked copies agree on it. |

### Internet Config: name, servers and proxies

Internet Config's preferences come from the Internet settings, set for
every application in the shell's Defaults (Internet and Proxies) or with
`bluebox set --default`, and per application over those. Programs that use
Internet Config (Internet Explorer, Netscape 4, Fetch, mail and news
readers) read them instead of asking; programs with settings of their own
keep using those.

```sh
bluebox set --default real-name "Pat Doe" email pat@example.com smtp-host smtp.example.com
bluebox set --default http-proxy proxy.example.com:8080 socks socks.example.com:1080 no-proxy "lan, example.com"
bluebox set "Some Program" http-proxy none      # no web proxy for this one
```

The settings are `real-name`, `email`, `organization`, `mail-account`
(user@host), `smtp-host`, `news-host`, `home-page`, `search-page`,
`http-proxy`, `ftp-proxy`, `gopher-proxy` and `socks` (host:port, or
`none`), `no-proxy` (domains, comma-separated) and `passive-ftp` (`on` or
`off`); `default` clears one. A URL a program opens through Internet
Config opens in the Mac's default browser.

MacTCP programs (Fetch, Eudora, IRC and Telnet clients) use the Mac's own
network with no setup, over Wi-Fi and VPNs too. MIDI Manager programs play
through the Mac's General MIDI synthesizer, or to and from CoreMIDI devices
when there are any.

### Looking inside a program

`bluebox inspect` lists a file's resources with what they hold: strings,
picture and icon sizes, sound formats, menus, dialogs and the `SIZE` flags.
With `--export` it saves them as files a modern Mac opens: pictures (drawn by
Blue Box's own QuickDraw) and icons as PNG, sounds as WAV, strings and text as
UTF-8.

```sh
bluebox inspect "SimpleText"
bluebox inspect "SimpleText" --export SimpleText-resources
bluebox inspect "SimpleText" --type snd --id 128 --export sounds
```

`--type` and `--id` choose resources; chosen ones with no modern format are
saved as they are (`.bin`).

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
