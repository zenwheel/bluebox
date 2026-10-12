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
import unicodedata

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


# ---------------------------------------------------------------------------
# MacRoman 0x80-0xFF, synthesised from each face's own ASCII glyphs: accented
# letters are the base letter plus a small accent; dashes, curly quotes,
# ellipsis and ligatures are built from existing glyphs.

ACCENTS = {
    "̀": ["#.", ".#"],                 # grave
    "́": [".#", "#."],                 # acute
    "̂": [".#.", "#.#"],               # circumflex
    "̃": [".#.#", "#.#."],             # tilde
    "̈": ["#.#"],                      # diaeresis
    "̊": [".#.", "#.#", ".#."],        # ring
}
CEDILLA = [".#", "##"]


def ink(g):
    """(left, top, rows) with rows padded to one width; left/top are pen-relative."""
    adv, ox, oy, rows = g
    w = max((len(r) for r in rows), default=0)
    return ox, oy, [r.ljust(w, ".") for r in rows]


def place(parts, adv):
    """Combine (left, top, rows) bitmaps into one glyph with advance adv."""
    parts = [p for p in parts if p[2]]
    if not parts:
        return (adv, 0, 0, [])
    left = min(p[0] for p in parts)
    top = min(p[1] for p in parts)
    right = max(p[0] + len(p[2][0]) for p in parts)
    bottom = max(p[1] + len(p[2]) for p in parts)
    grid = [["."] * (right - left) for _ in range(bottom - top)]
    for l, t, rows in parts:
        for r, line in enumerate(rows):
            for c, ch in enumerate(line):
                if ch == "#":
                    grid[t - top + r][l - left + c] = "#"
    return (adv, left, top, ["".join(r) for r in grid])


def dotless(face, g):
    """An i or j without its dot: drop rows above the x-height."""
    xh = face["glyphs"][ord("x")][2]
    left, top, rows = ink(g)
    keep = [(top + r, line) for r, line in enumerate(rows) if top + r >= xh]
    return (g[0], left, keep[0][0], [line for _, line in keep])


def accented(face, base, mark):
    g = face["glyphs"].get(ord(base))
    if g is None:
        return None
    if base in "ij":
        g = dotless(face, g)
    left, top, rows = ink(g)
    width = len(rows[0]) if rows else 0
    if mark == "̧":
        acc = CEDILLA
        al = left + (width - len(acc[0])) // 2
        return place([(left, top, rows), (al, top + len(rows), acc)], g[0])
    acc = ACCENTS.get(mark)
    if acc is None:
        return None
    al = left + (width - len(acc[0]) + 1) // 2
    at = top - 1 - len(acc)
    at = max(at, -face["ascent"])           # capitals: keep inside the ascent
    if at + len(acc) >= top:                 # no room for a gap: squeeze the letter's top row
        rows = rows[1:]
        top += 1
    return place([(left, top, rows), (al, at, acc)], g[0])


def joined(face, chars, overlap=0):
    parts, pen = [], 0
    for ch in chars:
        g = face["glyphs"].get(ord(ch))
        if g is None:
            return None
        left, top, rows = ink(g)
        parts.append((pen + left, top, rows))
        pen += g[0] - overlap
    return place(parts, pen + overlap)


def dash(face, width):
    left, top, rows = ink(face["glyphs"][ord("-")])
    return (width + 1, 0, top, ["#" * width for _ in rows])


def flipped(face, ch):
    g = face["glyphs"].get(ord(ch))
    if g is None:
        return None
    left, top, rows = ink(g)
    # Turned over, hanging from the x-height.
    return (g[0], left, face["glyphs"][ord("x")][2], rows[::-1])


def bullet(face):
    xh = -face["glyphs"][ord("x")][2]
    n = 3 if face["ascent"] < 11 else 4
    rows = [".##.", "####", "####", ".##."] if n == 4 else [".#.", "###", ".#."]
    return (n + 2, 1, -(xh + n) // 2 - 1, rows)


def ring(d):
    """A circle outline d pixels across."""
    c = (d - 1) / 2
    rows = []
    for y in range(d):
        row = ""
        for x in range(d):
            r = ((x - c) ** 2 + (y - c) ** 2) ** 0.5
            row += "#" if abs(r - c) < 0.6 else "."
        rows.append(row)
    return rows


def overlay(base, inner):
    """inner centred inside base (both lists of equal-width strings)."""
    out = [list(r) for r in base]
    t = (len(base) - len(inner)) // 2
    l = (len(base[0]) - len(inner[0])) // 2
    for y, line in enumerate(inner):
        for x, ch in enumerate(line):
            if ch == "#":
                out[t + y][l + x] = "#"
    return ["".join(r) for r in out]


def symbol(face, ch):
    """Symbols with no ASCII parts, drawn to the face's cap and x-heights."""
    g = face["glyphs"]
    cap = -g[ord("H")][2]
    xh = -g[ord("x")][2]
    mid = -(xh + 1) // 2 - 1                # a row near the middle of the x-height
    if ch in "\u00A9\u00AE":
        d = cap + 1 if cap % 2 == 0 else cap
        inner = ([".##", "#..", ".##"] if ch == "\u00A9" else ["##.", "#.#", "##.", "#.#"]) if d >= 8 else \
                (["##", "#."] if ch == "\u00A9" else ["##", "#."])
        rows = overlay(ring(d), inner)
        return (d + 2, 1, -cap, rows)
    if ch == "\u2122":
        rows = ["###.#...#", ".#..##.##", ".#..#.#.#"]
        return (11, 1, -cap, rows)
    if ch == "\u00B0":
        return (5, 1, -cap, [".#.", "#.#", ".#."])
    if ch == "\u00B1":
        return (7, 1, -xh, ["..#..", "..#..", "#####", "..#..", "..#..", ".....", "#####"][: xh + 1])
    if ch == "\u00F7":
        return (7, 1, mid - 2, ["..#..", ".....", "#####", ".....", "..#.."])
    if ch == "\u00AC":
        return (7, 1, mid, ["#####", "....#"])
    if ch == "\u2020":
        return (7, 1, -cap, ["..#..", "#####"] + ["..#.."] * (cap - 2))
    if ch == "\u2021":
        return (7, 1, -cap, ["..#..", "#####"] + ["..#.."] * (cap - 4) + ["#####", "..#.."])
    if ch == "\u00B5":
        left, top, rows = ink(g[ord("u")])
        return (g[ord("u")][0], left, top, rows + [rows[-1][0] + "." * (len(rows[0]) - 1)] * 2)
    if ch == "\u00A2":
        left, top, rows = ink(g[ord("c")])
        col = len(rows[0]) // 2
        stem = "." * col + "#" + "." * (len(rows[0]) - col - 1)
        return place([(left, top, rows), (left, top - 1, [stem]), (left, top + len(rows), [stem])], g[ord("c")][0])
    if ch in "\u00F8\u00D8":
        base = g[ord("o" if ch == "\u00F8" else "O")]
        left, top, rows = ink(base)
        h, w = len(rows), len(rows[0])
        slash = ["".join("#" if x == (w - 1) * (h - 1 - y) // max(h - 1, 1) else "." for x in range(w))
                 for y in range(h)]
        return place([(left, top, rows), (left, top, slash)], base[0])
    if ch == "\u00DF":
        made = joined(face, "B")
        if made:
            adv, left, top, rows = made
            # A tall B with a descender-less stem reads as sharp s at these sizes.
            return (adv, left, top, rows)
    return None


def macroman_glyphs(face):
    out = {}
    g = face["glyphs"]
    n_width = g[ord("n")][0] - 1
    m_width = g[ord("M")][0]
    for code in range(0x80, 0x100):
        ch = bytes([code]).decode("mac_roman")
        nfd = unicodedata.normalize("NFD", ch)
        made = None
        if len(nfd) == 2:
            made = accented(face, nfd[0], nfd[1])
        elif ch in "–":
            made = dash(face, n_width)
        elif ch == "—":
            made = dash(face, m_width)
        elif ch in "‘’‚":
            made = g.get(ord("'"))
        elif ch in "“”„":
            made = g.get(ord('"'))
        elif ch == "…":
            made = joined(face, "...")
        elif ch == "•":
            made = bullet(face)
        elif ch == " ":
            made = g.get(ord(" "))
        elif ch == "ı":
            made = dotless(face, g[ord("i")])
        elif ch in "ﬁﬂ":
            made = joined(face, "fi" if ch == "ﬁ" else "fl", overlap=1)
        elif ch in "ÆæŒœ":
            made = joined(face, {"Æ": "AE", "æ": "ae", "Œ": "OE", "œ": "oe"}[ch], overlap=1)
        elif ch == "«":
            made = joined(face, "<<", overlap=2)
        elif ch == "»":
            made = joined(face, ">>", overlap=2)
        elif ch == "‹":
            made = g.get(ord("<"))
        elif ch == "›":
            made = g.get(ord(">"))
        elif ch == "¡":
            made = flipped(face, "!")
        elif ch == "¿":
            made = flipped(face, "?")
        else:
            made = symbol(face, ch)
        if made is not None:
            out[code] = made
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
            more = macroman_glyphs(face)
            if fam == 0:
                more.update(extra)
            data = build_nfnt(face, more)
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
