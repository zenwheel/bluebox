#!/usr/bin/env python3
"""Build the standard System resources applications expect to find.

The cursors every System file had (CURS 1-4: I-beam, cross, plus, watch)
and the alert icons (ICON 0-2: stop, note, caution), drawn here as ASCII art
(our own designs in the classic manner) and written as byte arrays to
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
    parts.append("\nconst sysres_entry sysres[] = {\n")
    for t, rid, sym in table:
        parts.append(f"    {{ 0x{int.from_bytes(t.encode(), 'big'):08X}, {rid}, {sym}, sizeof {sym} }}, /* '{t}' */\n")
    parts.append("};\nconst unsigned sysres_count = sizeof sysres / sizeof sysres[0];\n")
    with open(OUT, "w") as f:
        f.write("".join(parts))
    print(f"wrote {OUT}: {len(table)} resources")


if __name__ == "__main__":
    main()
