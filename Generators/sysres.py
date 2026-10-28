#!/usr/bin/env python3
"""Build the standard System resources applications expect to find.

The cursors every System file had (CURS 1-4: I-beam, cross, plus, watch),
the alert icons (ICON 0-2: stop, note, caution), drawn here as ASCII art
(our own designs in the classic manner), and the Standard File dialogs
(DLOG/DITL -4000, -3999, -6042, -6043) with the item numbers applications
rely on when they customise them, the U.S. keyboard layout and the U.S.
international resources (itl0, itl1, itl2, itl4, itlb, itlc; intlres.py),
written as byte arrays to Sources/Toolbox/sysres_gen.c for the
compatibility System file.

    python3 Generators/sysres.py
"""

import os

import intlres

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT = os.path.join(ROOT, "Sources", "Toolbox", "sysres_gen.c")

# Cursors: '#' black, 'o' white (in the mask only), '.' transparent.
CURSORS = {
    1: ("I-beam", (6, 4), [
        "..##.##.........",
        "....#...........",
        "....#...........",
        "....#...........",
        "....#...........",
        "....#...........",
        "....#...........",
        "....#...........",
        "....#...........",
        "....#...........",
        "....#...........",
        "...###..........",
        "....#...........",
        "....#...........",
        "..##.##.........",
        "................",
    ]),
    2: ("cross", (7, 7), [
        ".......#........",
        ".......#........",
        ".......#........",
        ".......#........",
        ".......#........",
        ".......#........",
        "................",
        "######...######.",
        "................",
        ".......#........",
        ".......#........",
        ".......#........",
        ".......#........",
        ".......#........",
        ".......#........",
        "................",
    ]),
    3: ("plus", (7, 7), [
        ".....oooo.......",
        ".....o##o.......",
        ".....o##o.......",
        ".....o##o.......",
        ".....o##o.......",
        "ooooooo##ooooooo",
        "o##############o",
        "o##############o",
        "ooooooo##ooooooo",
        ".....o##o.......",
        ".....o##o.......",
        ".....o##o.......",
        ".....o##o.......",
        ".....oooo.......",
        "................",
        "................",
    ]),
    4: ("watch", (8, 8), [
        "....########....",
        "....########....",
        "....########....",
        "...#oooooooo#...",
        "..#oooo#ooooo#..",
        ".#ooooo#oooooo#.",
        ".#ooooo#oooooo##",
        ".#ooooo####ooo##",
        ".#oooooooooooo#.",
        ".#oooooooooooo#.",
        "..#oooooooooo#..",
        "...#oooooooo#...",
        "....########....",
        "....########....",
        "....########....",
        "................",
    ]),
}

# Alert icons, 32x32: '#' black.
ICONS = {
    0: ("stop", [
        "..........############..........",
        ".........#oooooooooooo#.........",
        "........#oooooooooooooo#........",
        ".......#oooooooooooooooo#.......",
        "......#oooooooooooooooooo#......",
        ".....#oooooooooooooooooooo#.....",
        "....#oooooooooooooooooooooo#....",
        "...#oooooooooooooooooooooooo#...",
        "..#oooooooooooooooooooooooooo#..",
        ".#oooooooooooooooooooooooooooo#.",
        "#oooooooooooooooooooooooooooooo#",
        "#oooooooooooooooooooooooooooooo#",
        "#ooo####oo######oo####oo#####oo#",
        "#oo#ooooooooo#oooo#oo#oo#oooo#o#",
        "#oo#ooooooooo#ooo#oooo#o#oooo#o#",
        "#ooo####ooooo#ooo#oooo#o#####oo#",
        "#ooooooo#oooo#ooo#oooo#o#ooooooo#"[:32],
        "#ooooooo#oooo#oooo#oo#oo#ooooooo#"[:32],
        "#oo#####ooooo#ooooo##ooo#oooooo#",
        "#oooooooooooooooooooooooooooooo#",
        "#oooooooooooooooooooooooooooooo#",
        ".#oooooooooooooooooooooooooooo#.",
        "..#oooooooooooooooooooooooooo#..",
        "...#oooooooooooooooooooooooo#...",
        "....#oooooooooooooooooooooo#....",
        ".....#oooooooooooooooooooo#.....",
        "......#oooooooooooooooooo#......",
        ".......#oooooooooooooooo#.......",
        "........#oooooooooooooo#........",
        ".........#oooooooooooo#.........",
        "..........############..........",
        "................................",
    ]),
    1: ("note", [
        "................................",
        "...........########.............",
        ".........##oooooooo##...........",
        "........#oooooooooooo#..........",
        ".......#oooooooooooooo#.........",
        "......#oooooooooooooooo#........",
        "......#oooooo##oooooooo#........",
        ".....#ooooooo##ooooooooo#.......",
        ".....#oooooooooooooooooo#.......",
        ".....#oooooo###ooooooooo#.......",
        ".....#ooooooo##ooooooooo#.......",
        ".....#ooooooo##ooooooooo#.......",
        "......#oooooo##oooooooo#........",
        "......#ooooo####ooooooo#........",
        ".......#oooooooooooooo#.........",
        "........#oooooooooooo#..........",
        ".........###oooooo###...........",
        "...........#ooooo#..............",
        "...........#oooo#...............",
        "..........#oooo#................",
        "..........#ooo#.................",
        ".........#ooo#..................",
        ".........#o##...................",
        "........##......................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
    ]),
    2: ("caution", [
        "...............##...............",
        "..............#oo#..............",
        "..............#oo#..............",
        ".............#oooo#.............",
        ".............#oooo#.............",
        "............#oooooo#............",
        "............#oooooo#............",
        "...........#ooo##ooo#...........",
        "...........#ooo##ooo#...........",
        "..........#oooo##oooo#..........",
        "..........#oooo##oooo#..........",
        ".........#ooooo##ooooo#.........",
        ".........#ooooo##ooooo#.........",
        "........#oooooo##oooooo#........",
        "........#oooooo##oooooo#........",
        ".......#ooooooo##ooooooo#.......",
        ".......#ooooooo##ooooooo#.......",
        "......#oooooooo##oooooooo#......",
        "......#oooooooooooooooooo#......",
        ".....#ooooooooooooooooooooo#....."[:32],
        ".....#ooooooooo##ooooooooo#.....",
        "....#oooooooooo##oooooooooo#....",
        "....#oooooooooooooooooooooo#....",
        "...#oooooooooooooooooooooooo#...",
        "...##########################...",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
    ]),
}


# Dialog items: (type, (top, left, bottom, right), text).
USER, BUTTON, STAT, EDIT, DISABLED = 0, 4, 8, 16, 128

DIALOGS = {
    # System 7 SFGetFile / CustomGetFile
    -6042: ((0, 0, 200, 344), [
        (BUTTON, (135, 252, 155, 332), "Open"),
        (BUTTON, (104, 252, 124, 332), "Cancel"),
        (USER | DISABLED, (0, 0, 0, 0), ""),            # balloon help
        (USER | DISABLED, (29, 248, 49, 344), ""),      # volume
        (BUTTON, (56, 252, 76, 332), "Eject"),
        (BUTTON, (79, 252, 99, 332), "Desktop"),
        (USER, (29, 12, 159, 230), ""),                 # file list
        (USER | DISABLED, (6, 12, 25, 230), ""),        # folder pop-up
        (USER | DISABLED, (91, 251, 92, 333), ""),      # divider
    ]),
    # System 7 SFPutFile / CustomPutFile
    -6043: ((0, 0, 188, 344), [
        (BUTTON, (161, 252, 181, 332), "Save"),
        (BUTTON, (130, 252, 150, 332), "Cancel"),
        (USER | DISABLED, (0, 0, 0, 0), ""),
        (USER | DISABLED, (8, 235, 24, 337), ""),
        (BUTTON, (32, 252, 52, 332), "Eject"),
        (BUTTON, (60, 252, 80, 332), "Desktop"),
        (USER, (29, 12, 127, 230), ""),
        (USER | DISABLED, (3, 12, 23, 230), ""),
        (USER | DISABLED, (88, 251, 89, 333), ""),
        (EDIT, (157, 15, 173, 227), ""),                # file name
        (STAT | DISABLED, (136, 15, 152, 227), ""),     # prompt
        (USER | DISABLED, (101, 252, 121, 332), ""),    # new folder
    ]),
    # The original SFGetFile
    -4000: ((0, 0, 200, 348), [
        (BUTTON, (138, 256, 156, 336), "Open"),
        (USER | DISABLED, (1152, 59, 1232, 77), ""),
        (BUTTON, (163, 256, 181, 336), "Cancel"),
        (USER | DISABLED, (39, 252, 59, 347), ""),      # volume
        (BUTTON, (68, 256, 86, 336), "Eject"),
        (BUTTON, (93, 256, 111, 336), "Drive"),
        (USER, (39, 12, 185, 230), ""),                 # file list
        (USER | DISABLED, (39, 229, 185, 246), ""),     # its scroll bar
        (USER | DISABLED, (124, 252, 125, 340), ""),    # divider
        (USER | DISABLED, (11, 12, 30, 230), ""),       # folder pop-up
    ]),
    # The original SFPutFile
    -3999: ((0, 0, 184, 304), [
        (BUTTON, (132, 218, 150, 288), "Save"),
        (BUTTON, (158, 218, 176, 288), "Cancel"),
        (STAT | DISABLED, (112, 14, 128, 197), ""),     # prompt
        (USER | DISABLED, (8, 198, 24, 302), ""),       # volume
        (BUTTON, (32, 217, 50, 287), "Eject"),
        (BUTTON, (58, 217, 76, 287), "Drive"),
        (EDIT, (136, 17, 152, 197), ""),                # file name
        (USER | DISABLED, (29, 14, 127, 197), ""),      # file list
        (USER | DISABLED, (6, 14, 25, 197), ""),        # folder pop-up
        (USER | DISABLED, (89, 209, 90, 297), ""),      # divider
    ]),
}


# ---------------------------------------------------------------------------
# Finder icon families (ICN#/ics# with icl4/icl8/ics4/ics8), our own designs.
# Drawn on a small canvas; letters are colours:
#   k black, w white, l light grey, m mid grey, d dark grey,
#   b folder blue (light), B folder blue (dark)

COLOURS = {  # letter: (8-bit standard clut index, 4-bit index, 1-bit)
    "k": (255, 15, 1), "w": (0, 0, 0), "l": (246, 12, 0), "m": (248, 13, 0), "d": (251, 14, 1),
    "b": (42, 12, 0), "B": (84, 6, 0),
}


class Canvas:
    def __init__(self, n):
        self.n = n
        self.px = [["." for _ in range(n)] for _ in range(n)]

    def set(self, x, y, c):
        if 0 <= x < self.n and 0 <= y < self.n:
            self.px[y][x] = c

    def fill(self, l, t, r, b, c):
        for y in range(t, b):
            for x in range(l, r):
                self.set(x, y, c)

    def frame(self, l, t, r, b, c="k", inside=None):
        if inside:
            self.fill(l, t, r, b, inside)
        for x in range(l, r):
            self.set(x, t, c)
            self.set(x, b - 1, c)
        for y in range(t, b):
            self.set(l, y, c)
            self.set(r - 1, y, c)

    def hline(self, l, r, y, c="k"):
        for x in range(l, r):
            self.set(x, y, c)

    def vline(self, x, t, b, c="k"):
        for y in range(t, b):
            self.set(x, y, c)

    def rows(self):
        return ["".join(r) for r in self.px]


def document(n):
    c = Canvas(n)
    if n == 32:
        l, t, r, b, fold = 6, 1, 26, 31, 7
    else:
        l, t, r, b, fold = 3, 0, 13, 16, 4
    c.frame(l, t, r, b, "k", "w")
    for i in range(fold):  # dog-ear
        c.hline(r - fold + i, r, t + i, ".")
        c.set(r - fold + i, t + i, "k")
    c.vline(r - fold, t, t + fold, "k")
    c.hline(r - fold, r, t + fold - 1 + 1, "k")
    c.fill(r - fold + 1, t + 1, r - fold + 2, t + fold, "l")
    return c


def folder(n, fill="b", tab="B", open_=False):
    c = Canvas(n)
    if n == 32:
        c.frame(1, 6, 13, 10, "k", tab)        # tab
        c.frame(1, 9, 31, 28, "k", fill)       # body
        c.hline(2, 30, 10, "w")
        if open_:
            c.fill(2, 11, 30, 27, "m")
    else:
        c.frame(0, 2, 7, 5, "k", tab)
        c.frame(0, 4, 16, 14, "k", fill)
        c.hline(1, 15, 5, "w")
        if open_:
            c.fill(1, 6, 15, 13, "m")
    return c


def floppy(n):
    c = Canvas(n)
    if n == 32:
        c.frame(3, 3, 29, 29, "k", "d")
        c.frame(9, 3, 23, 12, "k", "l")      # shutter
        c.fill(17, 5, 20, 10, "k")
        c.frame(7, 17, 25, 29, "k", "w")     # label
        for y in (20, 23, 26):
            c.hline(9, 23, y, "m")
    else:
        c.frame(1, 1, 15, 15, "k", "d")
        c.frame(4, 1, 12, 6, "k", "l")
        c.fill(9, 2, 11, 5, "k")
        c.frame(3, 9, 13, 15, "k", "w")
    return c


def application(n):
    c = Canvas(n)
    h = n // 2
    r = h - (2 if n == 32 else 1)
    for y in range(n):
        span = r - abs(y - h + 0.5) + 0.5
        if span <= 0:
            continue
        span = int(span)
        for x in range(h - span, h + span):
            c.set(x, y, "l")
        c.set(h - span, y, "k")
        c.set(h + span - 1, y, "k")
    for x in range(n):  # top and bottom points
        pass
    # a hand-written "A" mark in the middle
    if n == 32:
        c.fill(13, 12, 19, 20, "B")
        c.frame(13, 12, 19, 20, "k")
    else:
        c.fill(6, 6, 10, 10, "B")
    return c


def hard_disk(n):
    c = Canvas(n)
    if n == 32:
        c.frame(1, 10, 31, 23, "k", "l")
        c.hline(2, 30, 11, "w")
        c.hline(2, 30, 21, "m")
        c.fill(4, 17, 6, 19, "B")            # light
        c.hline(20, 28, 18, "d")             # slot
    else:
        c.frame(0, 5, 16, 12, "k", "l")
        c.set(2, 9, "B")
        c.hline(9, 14, 9, "d")
    return c


def trash(n, full=False):
    c = Canvas(n)
    if n == 32:
        c.frame(9, 2, 23, 5, "k", "l")       # handle
        c.frame(4, 5, 28, 9, "k", "l")       # lid
        c.frame(6, 9, 26, 31, "k", "l")      # can
        for x in (10, 14, 18, 22):
            c.vline(x, 11, 29, "d")
        if full:
            c.frame(3, 3, 12, 8, "k", "w")
            c.frame(20, 1, 30, 6, "k", "w")
    else:
        c.frame(5, 0, 11, 2, "k", "l")
        c.frame(2, 2, 14, 4, "k", "l")
        c.frame(3, 4, 13, 16, "k", "l")
        for x in (5, 8, 11):
            c.vline(x, 6, 14, "d")
        if full:
            c.frame(1, 0, 6, 3, "k", "w")
    return c


def desktop(n):
    c = Canvas(n)
    if n == 32:
        c.frame(4, 3, 28, 24, "k", "l")      # monitor
        c.frame(7, 6, 25, 19, "k", "b")      # screen
        c.fill(8, 7, 24, 9, "w")             # menu bar
        c.frame(10, 24, 22, 29, "k", "m")    # stand
    else:
        c.frame(2, 1, 14, 12, "k", "l")
        c.frame(4, 3, 12, 9, "k", "b")
        c.frame(5, 12, 11, 15, "k", "m")
    return c


def system_folder(n):
    c = folder(n)
    if n == 32:
        c.frame(12, 13, 20, 25, "k", "l")    # a small Mac
        c.frame(13, 14, 19, 20, "k", "w")
    else:
        c.frame(6, 6, 10, 12, "k", "l")
    return c


ICON_FAMILIES = {
    -4000: document, -3999: folder, -3998: floppy, -3997: lambda n: folder(n, open_=True),
    -3996: application, -3995: hard_disk, -3993: trash, -3992: desktop,
    -3984: lambda n: trash(n, True), -3983: system_folder,
}
# The special folders (Apple Menu Items, Startup Items, Control Panels,
# Preferences, Extensions, Fonts …) look like plain folders here.
for fid in (-3982, -3981, -3980, -3979, -3978, -3977, -3976, -3975, -3974, -3973, -3968, -3965, -3960):
    ICON_FAMILIES[fid] = folder


def icon_family(draw):
    out = {}
    for n, mono, c4, c8 in ((32, "ICN#", "icl4", "icl8"), (16, "ics#", "ics4", "ics8")):
        rows = draw(n).rows()
        mono_rows = ["".join("#" if COLOURS.get(ch, (0, 0, 0))[2] else "." for ch in r) for r in rows]
        mask_rows = ["".join("." if ch == "." else "#" for ch in r) for r in rows]
        out[mono] = bits(mono_rows, n, "#") + bits(mask_rows, n, "#")
        b8 = bytearray()
        b4 = bytearray()
        for r in rows:
            v = [COLOURS[ch][1] if ch != "." else 0 for ch in r]
            b8 += bytes(COLOURS[ch][0] if ch != "." else 0 for ch in r)
            for i in range(0, n, 2):
                b4.append(v[i] << 4 | v[i + 1])
        out[c4] = bytes(b4)
        out[c8] = bytes(b8)
    return out


def be16(v):
    return (v & 0xFFFF).to_bytes(2, "big")


def pstr(text):
    b = text.encode("mac_roman")
    return bytes([len(b)]) + b


def ditl(items):
    out = bytearray(be16(len(items) - 1))
    for kind, (t, l, b, r), text in items:
        data = text.encode("mac_roman")
        out += bytes(4) + be16(t) + be16(l) + be16(b) + be16(r) + bytes([kind, len(data)]) + data
        if len(data) & 1:
            out.append(0)
    return bytes(out)


def dlog(rect, items_id):
    t, l, b, r = rect
    # bounds, procID dBoxProc, invisible, no close box, refCon, items, title
    out = be16(t) + be16(l) + be16(b) + be16(r) + be16(1) + bytes([0, 0, 0, 0]) + bytes(4) + be16(items_id)
    out += pstr("")
    if len(out) & 1:
        out += b"\0"
    return out


def bits(rows, width, on):
    out = bytearray()
    for r in rows:
        r = (r + "." * width)[:width]
        for x in range(0, width, 8):
            b = 0
            for k in range(8):
                if r[x + k] in on:
                    b |= 0x80 >> k
            out.append(b)
    return bytes(out)


def curs(rows, hot):
    data = bits(rows, 16, "#")
    mask = bits(rows, 16, "#o")
    v, h = hot
    return data + mask + bytes([0, v, 0, h])


def icon(rows):
    return bits(rows, 32, "#")


def c_array(name, data):
    lines = []
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",")
    return f"static const uint8_t {name}[{len(data)}] = {{\n" + "\n".join(lines) + "\n};\n"


# A U.S. keyboard layout ('KCHR' 0), the same table KeyTranslate uses
# (osutil.c): HyperCard looks for the script's KCHR before it takes a
# Return in the message box, and beeps without one.
KEY_PLAIN = (
    "asdfhgzxcv\0bqwer"
    "yt123465=97-80]o"
    "u[ip\rlj'k;\\,/nm."
    "\t `\b\0\x1b" + "\0" * 10 +
    "\0.\x1d*\0+\x1c\x1b\x1f\0\0/\x03\x1e-\0"
    "\0=01234567\089\0\0\0"
    "\x10\x10\x10\x10\x10\x10\0\x10\0\x10\0\x10\0\x10\0\x10"
    "\0\x10\x05\x01\x0b\x7f\x10\x04\x10\x0c\x10\x1c\x1d\x1f\x1e\0"
)


def kchr():
    assert len(KEY_PLAIN) == 128
    frm, to = "1234567890-=[]\\;',./`", "!@#$%^&*()_+{}|:\"<>?~"
    plain = bytes(ord(c) for c in KEY_PLAIN)
    shift = bytes(c - 32 if 97 <= c <= 122 else ord(to[frm.index(chr(c))]) if c and chr(c) in frm else c
                  for c in plain)
    caps = bytes(c - 32 if 97 <= c <= 122 else c for c in plain)
    control = bytes(c & 0x1F if 97 <= c <= 122 else c for c in plain)
    tables = [plain, shift, caps, control]
    # Modifier byte: bit 0 command, 1 shift, 2 caps lock, 3 option, 4 control.
    index = bytes(3 if m & 0x10 else 1 if m & 0x02 else 2 if m & 0x04 else 0 for m in range(256))
    return be16(2) + index + be16(len(tables)) + b"".join(tables) + be16(0)


def main():
    parts = ["/* Generated by Generators/sysres.py. Do not edit. */\n",
             "#include \"sysres_gen.h\"\n\n"]
    table = []
    for rid, (label, hot, rows) in CURSORS.items():
        assert len(rows) == 16, label
        parts.append(f"/* CURS {rid}: {label} */\n" + c_array(f"curs_{rid}", curs(rows, hot)))
        table.append(("CURS", rid, f"curs_{rid}"))
    for rid, (label, rows) in ICONS.items():
        assert len(rows) == 32, label
        parts.append(f"/* ICON {rid}: {label} */\n" + c_array(f"icon_{rid}", icon(rows)))
        table.append(("ICON", rid, f"icon_{rid}"))
    for rid, draw in ICON_FAMILIES.items():
        for typ, data in icon_family(draw).items():
            sym = f"{typ.replace('#', 'H')}_m{-rid}"
            parts.append(c_array(sym, data))
            table.append((typ, rid, sym))
    for rid, (rect, items) in DIALOGS.items():
        n = -rid
        parts.append(f"/* DLOG/DITL {rid}: Standard File */\n" + c_array(f"dlog_m{n}", dlog(rect, rid)))
        parts.append(c_array(f"ditl_m{n}", ditl(items)))
        table.append(("DLOG", rid, f"dlog_m{n}"))
        table.append(("DITL", rid, f"ditl_m{n}"))
    # PAT 16, the desktop pattern (deskPatID): the 50% gray every System had,
    # read by the General control panel and by programs that draw the desktop.
    parts.append("/* PAT 16: the desktop pattern */\n" + c_array("pat_16", bytes.fromhex("AA55AA55AA55AA55")))
    table.append(("PAT ", 16, "pat_16"))
    # PAT -15904, the Control Panel's own (owned by its DRVR 15): a sparse dot
    # pattern.  A cdev is called with CPanelID -15904 and some draw with
    # GetPattern(CPanelID): the System 6 Keyboard cdev dims its items so.
    parts.append("/* PAT -15904: the Control Panel's pattern */\n" + c_array("pat_m15904", bytes.fromhex("4000040040000400")))
    table.append(("PAT ", -15904, "pat_m15904"))
    parts.append("/* KCHR 0: U.S. keyboard layout */\n" + c_array("kchr_0", kchr()))
    table.append(("KCHR", 0, "kchr_0"))
    for typ, rid, label, data in intlres.resources():
        sym = f"{typ}_{rid}"
        parts.append(f"/* {typ} {rid}: {label} */\n" + c_array(sym, data))
        table.append((typ, rid, sym))
    parts.append("\nconst sysres_entry sysres[] = {\n")
    for t, rid, sym in table:
        parts.append(f"    {{ 0x{int.from_bytes(t.encode(), 'big'):08X}, {rid}, {sym}, sizeof {sym} }}, /* '{t}' */\n")
    parts.append("};\nconst unsigned sysres_count = sizeof sysres / sizeof sysres[0];\n")
    with open(OUT, "w") as f:
        f.write("".join(parts))
    print(f"wrote {OUT}: {len(table)} resources")


if __name__ == "__main__":
    main()
