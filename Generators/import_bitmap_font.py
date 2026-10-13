#!/usr/bin/env python3
"""Turn a third-party font into a BDF strike for Generators/fonts.py.

A development tool, run once when a font is added (it needs Pillow and
rsrcfork, which the build itself does not):

    import_bitmap_font.py nfnt <AppleDouble ._file> <NFNT id> <out.bdf>
        a classic Mac bitmap strike, as it is (all 256 MacRoman codes)
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
    if len(sys.argv) < 5 or sys.argv[1] not in ("nfnt", "ttf"):
        raise SystemExit(__doc__)
    kind, src, num, out = sys.argv[1:5]
    opts = dict(a.split("=", 1) for a in sys.argv[5:])
    if kind == "nfnt":
        ascent, descent, glyphs = from_nfnt(src, int(num))
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
