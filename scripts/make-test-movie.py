#!/usr/bin/env python3
"""Write flattened QuickTime movies with text and sprite tracks, for testing.

    python3 -I scripts/make-test-movie.py OUTPUT_DIRECTORY

Writes two self-contained movies (data fork only, 'mdat' then 'moov', the
classic version-0 atoms a QuickTime 2.x Mac reads; time scale 600):

  Captions.mov  A 160 x 120 video track (raw 16-bit frames, one a second,
                changing colour) with a text track of four timed captions
                below it: white on black, centred, with 'styl' style runs
                and an 'ftab', one caption highlighted ('hlit').  Text
                tracks need QuickTime 1.5.
  Sprites.mov   A 200 x 150 sprite track: a background sprite (layer 32767),
                a ball that crosses the stage and changes picture halfway
                (graphics mode transparent, magenta keyed out), and a square
                moving diagonally on a higher layer; one key frame and
                override samples ten times a second.  Over it, keyed text
                with a drop shadow.  Sprite tracks need QuickTime 2.1.

On a Mac the files want type 'MooV' and creator 'TVOD' (MoviePlayer).
"""
import os
import struct
import sys

SCALE = 600


def be16(v):
    return struct.pack(">H", v & 0xFFFF)


def be32(v):
    return struct.pack(">I", v & 0xFFFFFFFF)


def fixed(v):
    return be32(int(round(v * 65536)))


def atom(kind, *parts):
    body = b"".join(parts)
    return be32(8 + len(body)) + kind + body


def full(kind, *parts, flags=0):
    return atom(kind, be32(flags), *parts)


def pstr(s, size=None):
    b = bytes([len(s)]) + s.encode("mac_roman")
    return b if size is None else b.ljust(size, b"\0")


def matrix(a=1.0, d=1.0, tx=0.0, ty=0.0):
    """A MatrixRecord: a b u / c d v / tx ty w (Fixed, the third column Fract)."""
    return fixed(a) + fixed(0) + be32(0) + fixed(0) + fixed(d) + be32(0) + fixed(tx) + fixed(ty) + be32(0x40000000)


def rgb(r, g, b):
    return be16(r) + be16(g) + be16(b)


# ---- Pictures ----

def raw16(w, h, pixel):
    """'raw ' 16-bit: big-endian xRGB 1-5-5-5, rows top down."""
    out = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b = pixel(x, y)
            out += be16((r >> 3) << 10 | (g >> 3) << 5 | (b >> 3))
    return bytes(out)


def raw32(w, h, pixel):
    """'raw ' 32-bit: ARGB."""
    out = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b = pixel(x, y)
            out += bytes((255, r, g, b))
    return bytes(out)


def image_description(fmt, w, h, depth, data_size, name):
    """An ImageDescription (also a video sample description), no colour table."""
    return (be32(86) + fmt + be32(0) + be16(0) + be16(1) + be16(0) + be16(0) + b"appl"
            + be32(0) + be32(0x200) + be16(w) + be16(h) + fixed(72) + fixed(72) + be32(data_size)
            + be16(1) + pstr(name, 32) + be16(depth) + be16(0xFFFF))


# ---- Movie structure ----

class Track:
    def __init__(self, handler, name, width, height, samples, durations, desc, minf_header,
                 layer=0, tx=0.0, ty=0.0, keys=None):
        self.handler, self.name = handler, name
        self.width, self.height = width, height
        self.samples, self.durations, self.desc = samples, durations, desc
        self.minf_header, self.layer, self.tx, self.ty, self.keys = minf_header, layer, tx, ty, keys
        self.offsets = []

    @property
    def duration(self):
        return sum(self.durations)

    def trak(self, track_id):
        tkhd = full(b"tkhd", be32(0), be32(0), be32(track_id), be32(0), be32(self.duration), be32(0), be32(0),
                    be16(self.layer), be16(0), be16(0), be16(0), matrix(tx=self.tx, ty=self.ty),
                    fixed(self.width), fixed(self.height), flags=0xF)
        elst = full(b"elst", be32(1), be32(self.duration), be32(0), fixed(1))
        mdhd = full(b"mdhd", be32(0), be32(0), be32(SCALE), be32(self.duration), be16(0), be16(0))
        hdlr = full(b"hdlr", b"mhlr", self.handler, b"appl", be32(0), be32(0), pstr(self.name))
        dhlr = full(b"hdlr", b"dhlr", b"alis", b"appl", be32(0), be32(0), pstr("Apple Alias Data Handler"))
        dinf = atom(b"dinf", full(b"dref", be32(1), full(b"alis", flags=1)))
        # durations, run-length coded
        runs = []
        for d in self.durations:
            if runs and runs[-1][1] == d:
                runs[-1][0] += 1
            else:
                runs.append([1, d])
        stbl = [full(b"stsd", be32(1), self.desc),
                full(b"stts", be32(len(runs)), *(be32(n) + be32(d) for n, d in runs))]
        if self.keys is not None:
            stbl.append(full(b"stss", be32(len(self.keys)), *(be32(k) for k in self.keys)))
        stbl += [full(b"stsc", be32(1), be32(1), be32(1), be32(1)),
                 full(b"stsz", be32(0), be32(len(self.samples)), *(be32(len(s)) for s in self.samples)),
                 full(b"stco", be32(len(self.offsets)), *(be32(o) for o in self.offsets))]
        minf = atom(b"minf", self.minf_header, dhlr, dinf, atom(b"stbl", *stbl))
        return atom(b"trak", tkhd, atom(b"edts", elst), atom(b"mdia", mdhd, hdlr, minf))


def vmhd():
    return full(b"vmhd", be16(0x40), be16(0x8000), be16(0x8000), be16(0x8000), flags=1)


def gmhd():
    return atom(b"gmhd", full(b"gmin", be16(0x40), be16(0x8000), be16(0x8000), be16(0x8000), be16(0), be16(0)))


def write_movie(path, tracks):
    """The samples in 'mdat' (one a chunk), then 'moov'."""
    data = bytearray()
    for t in tracks:
        t.offsets = []
        for s in t.samples:
            t.offsets.append(8 + len(data))
            data += s
    mdat = be32(8 + len(data)) + b"mdat" + bytes(data)
    duration = max(t.duration for t in tracks)
    mvhd = full(b"mvhd", be32(0), be32(0), be32(SCALE), be32(duration), fixed(1), be16(0x100), bytes(10),
                matrix(), be32(0), be32(0), be32(0), be32(0), be32(0), be32(0), be32(len(tracks) + 1))
    moov = atom(b"moov", mvhd, *(t.trak(i + 1) for i, t in enumerate(tracks)))
    with open(path, "wb") as f:
        f.write(mdat + moov)


# ---- Text ----

GENEVA, NEW_YORK, TIMES, HELVETICA = 3, 2, 20, 21
FONT_NAMES = {GENEVA: "Geneva", NEW_YORK: "New York", TIMES: "Times", HELVETICA: "Helvetica"}
BOLD, ITALIC, UNDERLINE = 1, 2, 4

dfClipToTextBox, dfDropShadow, dfKeyedText = 1 << 2, 1 << 12, 1 << 14


def text_description(flags, just, bg, box, font, size, color):
    """A TextDescription: flags, justification, background, text box, the
    default style (a ScrpSTElement) and the font's name."""
    style = be32(0) + be16(size + 4) + be16(size) + be16(font) + bytes((0, 0)) + be16(size) + rgb(*color)
    body = (b"text" + bytes(6) + be16(1) + be32(flags) + be32(just & 0xFFFFFFFF) + rgb(*bg)
            + b"".join(be16(v) for v in box) + style + pstr(FONT_NAMES[font]))
    return be32(4 + len(body)) + body


def text_sample(runs, hilite=None, shadow=None):
    """A caption from (text, font, face, size, colour) runs: the text, then
    'styl' (TextEdit's style scrap), 'ftab' and optional 'hlit', 'drpo'."""
    text = b"".join(r[0].encode("mac_roman") for r in runs)
    styl, at = be16(len(runs)), 0
    for s, font, face, size, color in runs:
        styl += be32(at) + be16(size + 4) + be16(size) + be16(font) + bytes((face, 0)) + be16(size) + rgb(*color)
        at += len(s.encode("mac_roman"))
    fonts = sorted({r[1] for r in runs})
    ftab = be16(len(fonts)) + b"".join(be16(f) + pstr(FONT_NAMES[f]) for f in fonts)
    out = be16(len(text)) + text + atom(b"styl", styl) + atom(b"ftab", ftab)
    if hilite:
        out += atom(b"hlit", be32(hilite[0]), be32(hilite[1])) + atom(b"hclr", rgb(0x6666, 0x6666, 0xCCCC))
    if shadow:
        out += atom(b"drpo", be16(shadow[0]), be16(shadow[1]))
    return out


WHITE, BLACK = (0xFFFF, 0xFFFF, 0xFFFF), (0, 0, 0)
RED, YELLOW, CYAN = (0xFFFF, 0x3333, 0x3333), (0xFFFF, 0xFFFF, 0), (0x6666, 0xFFFF, 0xFFFF)


def captions_movie(path):
    w, h = 160, 120
    frames, colours = [], [(40, 80, 200), (40, 160, 90), (200, 120, 40), (150, 50, 160),
                           (40, 80, 200), (40, 160, 90), (200, 120, 40), (150, 50, 160)]
    for i, (r, g, b) in enumerate(colours):
        def pixel(x, y, r=r, g=g, b=b, i=i):
            shade = 0.55 + 0.45 * y / h
            bar = (x // 20) == i
            return (255, 255, 255) if bar and y < 12 else (int(r * shade), int(g * shade), int(b * shade))
        frames.append(raw16(w, h, pixel))
    video = Track(b"vide", "Apple Video Media Handler", w, h, frames, [SCALE] * len(frames),
                  image_description(b"raw ", w, h, 16, 0, "None"), vmhd())
    th = 36
    captions = [
        text_sample([("Blue Box plays text tracks", GENEVA, 0, 12, WHITE)]),
        text_sample([("Bold, ", GENEVA, BOLD, 12, WHITE), ("italic", TIMES, ITALIC, 14, YELLOW),
                     (" and ", GENEVA, 0, 12, WHITE), ("red", HELVETICA, BOLD | UNDERLINE, 12, RED)]),
        text_sample([("A long caption wraps onto a second line in its box", GENEVA, 0, 12, CYAN)]),
        text_sample([("This word is highlighted", GENEVA, 0, 12, WHITE)], hilite=(10, 24)),
    ]
    text = Track(b"text", "Apple Text Media Handler", w, th, captions, [2 * SCALE] * 4,
                 text_description(0, 1, BLACK, (0, 0, th, w), GENEVA, 12, WHITE), gmhd(), ty=h)
    write_movie(path, [video, text])


# ---- Sprites ----

def qt_atom(kind, atom_id, children=None, data=b""):
    """A QT atom: size, type, ID, reserved, child count, reserved, then the
    children or the leaf data."""
    if isinstance(kind, int):
        kind = be32(kind)
    body = b"".join(children) if children is not None else data
    count = len(children) if children is not None else 0
    return be32(20 + len(body)) + kind + be32(atom_id) + be16(0) + be16(count) + be32(0) + body


def qt_container(children):
    """A QT atom container: 10 reserved bytes, the lock count, the 'sean' root."""
    return bytes(10) + be16(0) + qt_atom(b"sean", 1, children)


MATRIX, VISIBLE, LAYER, MODE, IMAGE = 1, 4, 5, 6, 100
srcCopy, transparent, ditherCopy = 0, 36, 64


def sprite_atom(sprite_id, x=None, y=None, visible=None, layer=None, image=None, mode=None):
    props = []
    if x is not None:
        props.append(qt_atom(MATRIX, 1, data=matrix(tx=x, ty=y)))
    if visible is not None:
        props.append(qt_atom(VISIBLE, 1, data=be16(1 if visible else 0)))
    if layer is not None:
        props.append(qt_atom(LAYER, 1, data=be16(layer)))
    if mode is not None:
        props.append(qt_atom(MODE, 1, data=be32(mode[0]) + rgb(*mode[1])))
    if image is not None:
        props.append(qt_atom(IMAGE, 1, data=be16(image)))
    return qt_atom(b"sprt", sprite_id, props)


def sprite_image(image_id, w, h, depth, data, name):
    desc = image_description(b"raw ", w, h, depth, len(data), name)
    return qt_atom(b"imag", image_id, [qt_atom(b"imda", 1, data=desc + data)])


def ball(colour):
    def pixel(x, y):
        dx, dy = x - 11.5, y - 11.5
        d = (dx * dx + dy * dy) ** 0.5
        if d > 11.5:
            return (255, 0, 255)  # keyed out
        light = max(0.0, 1 - ((dx + 4) ** 2 + (dy + 4) ** 2) ** 0.5 / 20)
        return tuple(min(255, int(c * (0.6 + 0.4 * light) + 120 * light ** 3)) for c in colour)
    return raw32(24, 24, pixel)


def sprites_movie(path):
    w, h = 200, 150
    background = raw16(w, h, lambda x, y: (30 + y // 2, 40 + y, 90 + y))
    square = raw32(16, 16, lambda x, y: (250, 220, 40) if 2 <= x < 14 and 2 <= y < 14 else (120, 80, 0))
    images = [sprite_image(1, w, h, 16, background, "Stage"), sprite_image(2, 24, 24, 32, ball((230, 40, 40)), "Red"),
              sprite_image(3, 24, 24, 32, ball((40, 90, 230)), "Blue"), sprite_image(4, 16, 16, 32, square, "Square")]
    steps = 40  # ten a second for four seconds
    keyed = (transparent, (0xFFFF, 0, 0xFFFF))

    def ball_at(i):
        return 4 + i * (w - 32) / (steps - 1), 60 + 30 * ((i % 20) - 10) ** 2 / 100 - 30

    def square_at(i):
        return 170 - i * 4, 10 + i * 3

    bx, by = ball_at(0)
    sx, sy = square_at(0)
    key = qt_container([
        sprite_atom(1, 0, 0, True, 32767, 1, (ditherCopy, BLACK)),
        sprite_atom(2, bx, by, True, 2, 2, keyed),
        sprite_atom(3, sx, sy, True, 1, 4, (srcCopy, BLACK)),
        qt_atom(b"dflt", 1, [qt_atom(b"imct", 1, images)]),
    ])
    samples = [key]
    for i in range(1, steps):
        bx, by = ball_at(i)
        sx, sy = square_at(i)
        samples.append(qt_container([sprite_atom(2, bx, by, image=3 if i >= steps // 2 else 2),
                                     sprite_atom(3, sx, sy)]))
    desc = be32(16) + b"sprt" + bytes(6) + be16(1)
    sprites = Track(b"sprt", "Apple Sprite Media Handler", w, h, samples, [SCALE // 10] * steps, desc, gmhd(),
                    keys=[1])
    th = 30
    words = [text_sample([("Sprites over a stage", GENEVA, BOLD, 12, YELLOW)]),
             text_sample([("...and the ball turns blue", GENEVA, BOLD, 12, YELLOW)])]
    text = Track(b"text", "Apple Text Media Handler", w, th, words, [2 * SCALE] * 2,
                 text_description(dfKeyedText | dfDropShadow, 1, BLACK, (0, 0, th, w), GENEVA, 12, YELLOW),
                 gmhd(), layer=-1, ty=h - th)
    write_movie(path, [sprites, text])


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    captions_movie(os.path.join(out, "Captions.mov"))
    sprites_movie(os.path.join(out, "Sprites.mov"))
    for name in ("Captions.mov", "Sprites.mov"):
        print(os.path.join(out, name), os.path.getsize(os.path.join(out, name)), "bytes")


if __name__ == "__main__":
    main()
