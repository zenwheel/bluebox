#!/usr/bin/env python3
"""Build the standard System resources applications expect to find.

The cursors every System file had (CURS 1-4: I-beam, cross, plus, watch),
the alert icons (ICON 0-2: stop, note, caution), drawn here as ASCII art
(our own designs in the classic manner), and the Standard File dialogs
(DLOG/DITL -4000, -3999, -6042, -6043) with the item numbers applications
rely on when they customise them, written as byte arrays to
Sources/Toolbox/sysres_gen.c for the compatibility System file.

    python3 Generators/sysres.py
"""

import os

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
    for rid, (rect, items) in DIALOGS.items():
        n = -rid
        parts.append(f"/* DLOG/DITL {rid}: Standard File */\n" + c_array(f"dlog_m{n}", dlog(rect, rid)))
        parts.append(c_array(f"ditl_m{n}", ditl(items)))
        table.append(("DLOG", rid, f"dlog_m{n}"))
        table.append(("DITL", rid, f"ditl_m{n}"))
    parts.append("\nconst sysres_entry sysres[] = {\n")
    for t, rid, sym in table:
        parts.append(f"    {{ 0x{int.from_bytes(t.encode(), 'big'):08X}, {rid}, {sym}, sizeof {sym} }}, /* '{t}' */\n")
    parts.append("};\nconst unsigned sysres_count = sizeof sysres / sizeof sysres[0];\n")
    with open(OUT, "w") as f:
        f.write("".join(parts))
    print(f"wrote {OUT}: {len(table)} resources")


if __name__ == "__main__":
    main()
