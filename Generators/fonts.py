#!/usr/bin/env python3
"""Build the compatibility System file's bitmap fonts.

Reads the OFL bitmap faces in Resources/Fonts/systemless/*.rs and writes
Sources/Toolbox/sysfonts_gen.c: NFNT strikes and FOND families, as byte
arrays, plus a table the machine adds to the System file at boot.

    python3 Generators/fonts.py

Fonts take the classic family numbers (0 system, 2 New York, 3 Geneva,
4 Monaco) so apps that ask for those families by number get them, and the
strike IDs follow the old FONT numbering (family * 128 + size).
"""

import os
import re
import struct

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SRC = os.path.join(ROOT, "Resources", "Fonts", "systemless")
OUT = os.path.join(ROOT, "Sources", "Toolbox", "sysfonts_gen.c")

FAMILIES = [
    # family id, our name, [(source file, size)]
    (0, "Classic System", [("chicago9", 9), ("chicago12", 12)]),
    (2, "Classic Serif", [("newyork12", 12), ("newyork14", 14), ("newyork18", 18)]),
    (3, "Classic Sans", [("geneva9", 9), ("geneva10", 10), ("geneva12", 12), ("geneva14", 14),
                         ("geneva18", 18), ("geneva24", 24)]),
    (4, "Classic Mono", [("monaco9", 9), ("monaco10", 10), ("monaco12", 12)]),
]

# Glyphs for low character codes the Menu Manager and Finder use.
# 0x11 command key, 0x12 check mark (from menu_symbols.rs), 0x13 diamond,
# 0x14 the first menu's title glyph (an original neutral mark, not a logo).
OWN_GLYPHS = {
    0x13: (9, 1, -8, [
        "...#...",
        "..###..",
        ".#####.",
        "#######",
        ".#####.",
        "..###..",
        "...#...",
    ]),
    0x14: (13, 1, -10, [
        ".....###...",
        "....#####..",
        "...#######.",
        "...###.###.",
        "..####.####",
        "..#########",
        "..#########",
        "...#######.",
        "...#######.",
        "....#####..",
    ]),
}

G_RE = re.compile(r'g!\(\s*(-?\d+)\s*,\s*\(\s*(-?\d+)\s*,\s*(-?\d+)\s*\)\s*,?(.*?)\)\s*,\s*(?://[^\n]*)?\n', re.S)


def parse_face(name):
    with open(os.path.join(SRC, name + ".rs")) as f:
        text = f.read()
    m = re.search(r"FontMetrics\s*\{\s*ascent:\s*(\d+),\s*descent:\s*(\d+),\s*wid_max:\s*(\d+),\s*leading:\s*(\d+)", text)
    ascent, descent, widmax, leading = (int(x) for x in m.groups())
    body = text[text.index("_SRC: &[GlyphSrc] = &["):]
    glyphs = []
    for g in G_RE.finditer(body):
        adv, ox, oy, rest = int(g.group(1)), int(g.group(2)), int(g.group(3)), g.group(4)
        rows = re.findall(r'"([#.]*)"', rest)
        glyphs.append((adv, ox, oy, rows))
    if len(glyphs) != 95:
        raise SystemExit(f"{name}: found {len(glyphs)} glyphs, expected 95")
    return dict(ascent=ascent, descent=descent, widmax=widmax, leading=leading,
                glyphs={0x20 + i: g for i, g in enumerate(glyphs)})


def parse_symbols():
    with open(os.path.join(SRC, "menu_symbols.rs")) as f:
        text = f.read()
    out = {}
    for const, code in (("COMMAND_KEY_SRC", 0x11), ("CHECKMARK_SRC", 0x12)):
        part = text[text.index(const):]
        g = re.search(r'g!\(\s*(-?\d+)\s*,\s*\(\s*(-?\d+)\s*,\s*(-?\d+)\s*\)\s*,(.*?)\)\]', part, re.S)
        rows = re.findall(r'"([#.]*)"', g.group(4))
        out[code] = (int(g.group(1)), int(g.group(2)), int(g.group(3)), rows)
    return out


def missing_glyph(ascent):
    h = max(ascent - 2, 5)
    rows = ["#" * 6] + ["#....#"] * (h - 2) + ["#" * 6]
    return (8, 1, -h, rows)


def build_nfnt(face, extra):
    glyphs = dict(face["glyphs"])
    glyphs.update(extra)
    first, last = min(glyphs), max(glyphs)
    asc, desc = face["ascent"], face["descent"]
    height = asc + desc
    order = list(range(first, last + 1))
    miss = missing_glyph(asc)
    # Lay glyph images side by side in one strike.
    cols = []
    loc = []
    x = 0
    kern = min(0, min(g[1] for g in glyphs.values()))
    images = []
    for code in order + ["missing"]:
        g = miss if code == "missing" else glyphs.get(code)
        loc.append(x)
        if g is None:
            continue
        adv, ox, oy, rows = g
        w = max((len(r) for r in rows), default=0)
        images.append((x, oy, rows))
        x += w
    loc.append(x)
    row_words = (x + 15) // 16 or 1
    strike = [bytearray(row_words * 2) for _ in range(height)]
    for x0, oy, rows in images:
        for r, line in enumerate(rows):
            y = asc + oy + r
            if not 0 <= y < height:
                continue
            for c, ch in enumerate(line):
                if ch == "#":
                    px = x0 + c
                    strike[y][px >> 3] |= 0x80 >> (px & 7)
    ow = []
    widmax = 0
    rect_w = 0
    for code in order + ["missing"]:
        g = miss if code == "missing" else glyphs.get(code)
        if g is None:
            ow.append(0xFFFF)
            continue
        adv, ox, oy, rows = g
        w = max((len(r) for r in rows), default=0)
        ow.append(((ox - kern) << 8) | adv)
        widmax = max(widmax, adv)
        rect_w = max(rect_w, ox - kern + w)
    ow.append(0xFFFF)
    header_len = 26
    image = b"".join(bytes(r) for r in strike)
    loc_bytes = b"".join(struct.pack(">H", v) for v in loc)
    ow_at = header_len + len(image) + len(loc_bytes)
    owtloc = (ow_at - 16) // 2
    hdr = struct.pack(">Hhhhhhhhhhhhh",
                      0x9000,          # fontType: proportional NFNT
                      first, last,
                      max(widmax, face["widmax"]),
                      kern,
                      -desc,           # nDescent
                      rect_w, height,
                      owtloc,
                      asc, desc, face["leading"],
                      row_words)
    ow_bytes = b"".join(struct.pack(">H", v) for v in ow)
    return hdr + image + loc_bytes + ow_bytes


def build_fond(fam, name, sizes, metrics):
    asc, desc, lead, widmax = metrics
    hdr = struct.pack(">hhhhhhhhiii", 0, fam, 0x20, 0xFF, 0, 0, 0, 0, 0, 0, 0)
    hdr += b"\0" * 18 + b"\0" * 4 + struct.pack(">h", 2)
    assoc = struct.pack(">h", len(sizes) - 1)
    for size, rid in sizes:
        assoc += struct.pack(">hhh", size, 0, rid)
    return hdr + assoc


def c_array(name, data):
    lines = []
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",")
    return f"static const uint8_t {name}[{len(data)}] = {{\n" + "\n".join(lines) + "\n};\n"


def main():
    symbols = parse_symbols()
    extra = dict(symbols)
    extra.update(OWN_GLYPHS)
    parts = ["/* Generated by Generators/fonts.py from Resources/Fonts/systemless (SIL OFL 1.1).\n"
             " * Do not edit. Bitmap artwork Copyright (c) 2026 Ben Letchford. */\n\n"
             "#include \"sysfonts.h\"\n\n"]
    table = []
    for fam, name, faces in FAMILIES:
        sizes = []
        metrics = None
        for src, size in faces:
            face = parse_face(src)
            data = build_nfnt(face, extra if fam == 0 else {})
            rid = fam * 128 + size
            sym = f"nfnt_{fam}_{size}"
            parts.append(c_array(sym, data))
            table.append((0x4E464E54, rid, "", sym, len(data)))
            sizes.append((size, rid))
            if metrics is None:
                metrics = (face["ascent"], face["descent"], face["leading"], face["widmax"])
        fond = build_fond(fam, name, sizes, metrics)
        sym = f"fond_{fam}"
        parts.append(c_array(sym, fond))
        table.append((0x464F4E44, fam, name, sym, len(fond)))
    parts.append("const sysfont_res sysfonts[] = {\n")
    for t, rid, name, sym, n in table:
        parts.append(f'    {{ 0x{t:08X}u, {rid}, "{name}", {sym}, {n} }},\n')
    parts.append("};\nconst unsigned sysfont_count = sizeof sysfonts / sizeof sysfonts[0];\n")
    with open(OUT, "w") as f:
        f.write("".join(parts))
    print(f"wrote {OUT}: {len(table)} resources")


if __name__ == "__main__":
    main()
