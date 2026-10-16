#!/usr/bin/env python3
"""Turn a third-party font into a BDF strike for Generators/fonts.py.

A development tool, run once when a font is added (it needs Pillow and
rsrcfork, which the build itself does not):

    import_bitmap_font.py nfnt <AppleDouble ._file> <NFNT id> <out.bdf>
        a classic Mac bitmap strike, as it is (all 256 MacRoman codes)
    import_bitmap_font.py sf2 <font.sf2> 0 <out.bdf> [keepf0=1] [name=value...]
        a Squeak strike2 font (AccuFonts), as it is; glyph $F0, where
        AccuFonts keep their copyright notice as a bitmap (MacRoman's Apple
        logo), is left out unless keepf0=1
    import_bitmap_font.py ttf <font.ttf> <pixels> <out.bdf> [name=value...]
        a TrueType font drawn in black and white at one pixel size, for
        faces whose outlines sit on the pixel grid (ChicagoFLF at 12,
        FontStruct pixel fonts at their design size)

Options set the classic face's line metrics where the source's differ:
ascent=, descent= (in pixels; glyphs keep their place on the baseline),
leading=, and space= (the space's width).

The BDF's ENCODING values are MacRoman character codes.
"""

import io
import struct
import sys


def write_bdf(path, name, size, ascent, descent, glyphs, leading=0):
    """glyphs: {code: (advance, left, top, rows)} with top relative to the
    baseline (negative is above) and rows strings of '#' and '.'."""
    out = [
        "STARTFONT 2.1",
        f"FONT {name}",
        f"SIZE {size} 72 72",
        f"FONTBOUNDINGBOX {max((len(g[3][0]) if g[3] else 0) for g in glyphs.values())} {ascent + descent} 0 {-descent}",
        "STARTPROPERTIES 3",
        f"FONT_ASCENT {ascent}",
        f"FONT_DESCENT {descent}",
        f"X_CLASSIC_LEADING {leading}",
        "ENDPROPERTIES",
        f"CHARS {len(glyphs)}",
    ]
    for code in sorted(glyphs):
        adv, left, top, rows = glyphs[code]
        w = len(rows[0]) if rows else 0
        h = len(rows)
        out += [f"STARTCHAR c{code:02X}", f"ENCODING {code}", f"DWIDTH {adv} 0",
                f"BBX {w} {h} {left} {-(top + h)}", "BITMAP"]
        for r in rows:
            bits = r.ljust((w + 7) // 8 * 8, ".")
            out.append("".join(f"{int(bits[i:i + 8].replace('#', '1').replace('.', '0'), 2):02X}"
                               for i in range(0, len(bits), 8)) if w else "00")
        out.append("ENDCHAR")
    out.append("ENDFONT")
    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")


def trim(rows, left, top):
    """Drop blank rows and columns around a glyph image."""
    ys = [y for y, r in enumerate(rows) if "#" in r]
    if not ys:
        return left, 0, []
    xs = [x for r in rows for x, ch in enumerate(r) if ch == "#"]
    x0, x1, y0, y1 = min(xs), max(xs) + 1, ys[0], ys[-1] + 1
    return left + x0, top + y0, [r[x0:x1] for r in rows[y0:y1]]


def from_nfnt(sidecar, rid):
    import rsrcfork
    b = open(sidecar, "rb").read()
    n = struct.unpack(">H", b[24:26])[0]
    fork = None
    for i in range(n):
        eid, off, ln = struct.unpack(">III", b[26 + 12 * i:38 + 12 * i])
        if eid == 2:
            fork = b[off:off + ln]
    rf = rsrcfork.ResourceFile(io.BytesIO(fork))
    d = rf[b"NFNT"][rid].data_raw
    (_, first, last, _, kern, _, _, height, owtloc, ascent, descent, _, row_words) = struct.unpack(">Hhhhhhhhhhhhh", d[:26])
    strike = d[26:26 + row_words * 2 * height]
    count = last - first + 3
    loc = struct.unpack(f">{count}H", d[26 + len(strike):26 + len(strike) + 2 * count])
    ow = struct.unpack(f">{count}H", d[16 + owtloc * 2:16 + owtloc * 2 + 2 * count])
    glyphs = {}
    for c in range(last - first + 1):
        if ow[c] == 0xFFFF:
            continue
        x0, x1 = loc[c], loc[c + 1]
        rows = ["".join("#" if (strike[y * row_words * 2 + x // 8] >> (7 - x % 8)) & 1 else "."
                        for x in range(x0, x1)) for y in range(height)]
        left, top, rows = trim(rows, (ow[c] >> 8) + kern, -ascent)
        glyphs[first + c] = (ow[c] & 0xFF, left, top, rows)
    return ascent, descent, glyphs


def sf2_decode_int(b, o):
    v = b[o]
    if v <= 223:
        return v, o + 1
    if v <= 254:
        return (v - 224) * 256 + b[o + 1], o + 2
    return struct.unpack(">I", b[o + 1:o + 5])[0], o + 5


def from_sf2(path, keep_f0=False):
    """A Squeak strike2 font: header, x table, then a Form (depth, extent,
    offset, bitmap words, raw or compressed)."""
    b = open(path, "rb").read()
    typ, first, last, _maxw, ascent, descent, _size, _emph = struct.unpack(">8i", b[:32])
    if typ != 2:
        raise SystemExit(f"{path}: not a strike2 font")
    n = last + 3 - first
    xt = struct.unpack(f">{n}i", b[32:32 + 4 * n])
    o = 32 + 4 * n
    depth = b[o]
    w, h = struct.unpack(">2H", b[o + 1:o + 5])
    o += 9
    per_row = (w * depth + 31) // 32
    words = []
    if b[o] == 0x80:  # compressed: byte count, then word count and runs
        ln, p = sf2_decode_int(b, o + 1)
        data = b[p:p + ln]
        _count, q = sf2_decode_int(data, 0)
        while q < len(data) and len(words) < per_row * h:
            code, q = sf2_decode_int(data, q)
            run, op = code >> 2, code & 3
            if op == 0:
                words += [0] * run
            elif op == 1:
                words += [data[q] * 0x01010101] * run
                q += 1
            elif op == 2:
                words += [struct.unpack(">I", data[q:q + 4])[0]] * run
                q += 4
            else:
                words += list(struct.unpack(f">{run}I", data[q:q + 4 * run]))
                q += 4 * run
    else:
        ln = struct.unpack(">i", b[o:o + 4])[0]
        words = list(struct.unpack(f">{ln}I", b[o + 4:o + 4 + 4 * ln]))
    words += [0] * (per_row * h - len(words))
    rows = ["".join("#" if (words[y * per_row + x // 32] >> (31 - x % 32)) & 1 else "." for x in range(w))
            for y in range(h)]
    glyphs = {}
    for c in range(first, last + 1):
        x0, x1 = xt[c - first], xt[c - first + 1]
        if x1 <= x0 and c != 0x20:
            continue
        if c == 0xF0 and not keep_f0:
            continue
        left, top, img = trim([r[x0:x1] for r in rows], 0, -ascent)
        glyphs[c] = (x1 - x0, left, top, img)
    return ascent, descent, glyphs


def from_ttf(path, px):
    from PIL import Image, ImageDraw, ImageFont
    font = ImageFont.truetype(path, px)
    ascent, descent = font.getmetrics()
    glyphs = {}
    # A character the font lacks draws its .notdef box: compare with one
    # from the private use area, which no font here maps.
    def picture(ch):
        m = font.getmask(ch)
        return m.size, bytes(m)
    notdef = picture("\uF8FF\uE000"[1])
    for code in list(range(0x20, 0x7F)) + list(range(0x80, 0x100)):
        ch = bytes([code]).decode("mac_roman")
        if code != 0x20 and (not font.getmask(ch).getbbox() or picture(ch) == notdef):
            continue  # not in the font
        adv = round(font.getlength(ch))
        w = adv + px
        img = Image.new("1", (w + px, ascent + descent), 1)
        draw = ImageDraw.Draw(img)
        draw.fontmode = "1"
        draw.text((px, 0), ch, font=font, fill=0)
        rows = ["".join("#" if img.getpixel((x, y)) == 0 else "." for x in range(img.width))
                for y in range(img.height)]
        left, top, rows = trim(rows, -px, -ascent)
        glyphs[code] = (adv, left, top, rows)
    return ascent, descent, glyphs


def main():
    if len(sys.argv) < 5 or sys.argv[1] not in ("nfnt", "ttf", "sf2"):
        raise SystemExit(__doc__)
    kind, src, num, out = sys.argv[1:5]
    opts = dict(a.split("=", 1) for a in sys.argv[5:])
    if kind == "nfnt":
        ascent, descent, glyphs = from_nfnt(src, int(num))
    elif kind == "sf2":
        ascent, descent, glyphs = from_sf2(src, opts.pop("keepf0", "0") == "1")
    else:
        ascent, descent, glyphs = from_ttf(src, int(num))
    ascent = int(opts.get("ascent", ascent))
    descent = int(opts.get("descent", descent))
    leading = int(opts.get("leading", 0))
    if "space" in opts and 0x20 in glyphs:
        glyphs[0x20] = (int(opts["space"]),) + glyphs[0x20][1:]
    name = out.rsplit("/", 1)[-1].rsplit(".", 1)[0]
    write_bdf(out, name, int(num) if kind == "ttf" else ascent + descent, ascent, descent, glyphs, leading)
    print(f"{out}: {len(glyphs)} glyphs, ascent {ascent}, descent {descent}, leading {leading}")


if __name__ == "__main__":
    main()
