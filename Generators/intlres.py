"""The U.S. international resources of the System file.

'itl0' (numbers, short dates, times), 'itl1' (long dates, day and month
names, with the extended Itl1ExtRec), 'itl2' (character types, case
conversion, diacritical stripping, word and line breaks), 'itl4' (the
IntlTokenize tables, number parts, white space), 'itlb' (the Roman script
bundle naming them) and 'itlc' (the international configuration), in the
formats of Inside Macintosh: Text, appendix B.  Every real System file had
them and programs read them directly; the Text Utilities themselves are
native and do not run the 68k hooks here.

The MacRoman tables are derived from Unicode: each byte is decoded with
Python's mac_roman codec, and its case mapping, diacritical marks and
category come from unicodedata.  sysres.py writes them out.
"""

import unicodedata

ROMAN = [bytes([c]).decode("mac_roman") for c in range(256)]


def be16(v):
    return (v & 0xFFFF).to_bytes(2, "big")


def be32(v):
    return (v & 0xFFFFFFFF).to_bytes(4, "big")


def pstr(text):
    b = text.encode("mac_roman")
    return bytes([len(b)]) + b


def pad(b):
    return b + b"\0" if len(b) & 1 else b


def field(text, n):
    """A Pascal string in an n-byte field."""
    return (pstr(text) + bytes(n))[:n]


def raw(text, n):
    """Up to n bytes of text, zero filled."""
    return (text.encode("mac_roman") + bytes(n))[:n]


def roman(ch):
    """The MacRoman code of a one-character string, or None."""
    try:
        b = ch.encode("mac_roman")
    except UnicodeEncodeError:
        return None
    return b[0] if len(b) == 1 else None


# ---------------------------------------------------------------------------
# MacRoman character properties

# The spacing accents at the top of the character set (ˆ ˜ ¯ ˘ ˙ ˚ ¸ ˝ ˛ ˇ):
# symbols to CharacterType, but parts of a word when selecting.
ACCENTS = range(0xF6, 0x100)
BLANKS = b"\0\t "
# Punctuation of running text; the remaining non-letters are symbols.
PUNCT = "!\"'(),-./:;?[]{}«»…—“”‘’".encode("mac_roman")


def is_letter(c):
    """A Latin letter (not µ, π, Ω, the ordinal indicators or ƒ, which is
    the florin sign)."""
    ch = ROMAN[c]
    return ch != "ƒ" and unicodedata.category(ch).startswith("L") and "LATIN" in unicodedata.name(ch, "")


def to_upper(c):
    u = ROMAN[c].upper()
    return roman(u) if len(u) == 1 and roman(u) is not None else c


def to_lower(c):
    u = ROMAN[c].lower()
    return roman(u) if len(u) == 1 and roman(u) is not None else c


def strip_mark(c):
    """The letter without its diacritical mark: the base of its canonical
    decomposition, or for Ø, ø and ı (which have none) the letter its name
    is built on."""
    if not is_letter(c):
        return c
    ch = ROMAN[c]
    d = unicodedata.normalize("NFD", ch)
    if len(d) > 1 and roman(d[0]) is not None:
        return roman(d[0])
    name = unicodedata.name(ch).replace("DOTLESS ", "")
    if " WITH " in name:
        name = name[:name.index(" WITH ")]
    try:
        base = roman(unicodedata.lookup(name))
    except KeyError:
        base = None
    return base if base is not None else c


def char_type(c):
    """The 'itl2' type-list byte: CharacterType's type in bits 0-3, its
    class in bits 4-6, upper case in bit 7."""
    if is_letter(c):
        return (0x80 if unicodedata.category(ROMAN[c]) == "Lu" else 0) | (1 if c < 0x80 else 7)  # ASCII / Euro
    if c in BLANKS or c == 0x0D:
        return 0x30  # smPunctBlank
    if 0x30 <= c <= 0x39:
        return 0x10  # smPunctNumber
    if c in PUNCT:
        return 0x00  # smPunctNormal
    return 0x20      # smPunctSymbol


# ---------------------------------------------------------------------------
# 'itl0': Intl0Rec

def itl0():
    out = b".,;" + raw("$", 3)               # decimalPt, thousSep, listSep, currSym1-3
    out += bytes([0x80 | 0x40 | 0x10])       # currFmt: leading and trailing zeros, symbol first
    out += bytes([0])                        # dateOrder: mdy
    out += bytes([0])                        # shrtDateFmt: no century, no leading zeros
    out += b"/"                              # dateSep
    out += bytes([0xFF])                     # timeCycle: 12 hours, noon and midnight 12:00
    out += bytes([0x40 | 0x20])              # timeFmt: minutes and seconds with leading zeros
    out += raw(" AM", 4) + raw(" PM", 4)     # mornStr, eveStr
    out += b":"                              # timeSep
    out += bytes(8)                          # time1Suff-time8Suff
    out += bytes([0])                        # metricSys: no
    out += be16(0x0001)                      # intl0Vers: region 0 (U.S.), version 1
    assert len(out) == 32
    return out


# ---------------------------------------------------------------------------
# 'itl1': Intl1Rec and Itl1ExtRec

DAYS = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"]
MONTHS = ["January", "February", "March", "April", "May", "June", "July", "August", "September",
          "October", "November", "December"]


def string_table(strings):
    """An Itl1ExtRec table: a count, then Pascal strings."""
    return be16(len(strings)) + b"".join(pstr(s) for s in strings)


def itl1():
    out = b"".join(field(d, 16) for d in DAYS) + b"".join(field(m, 16) for m in MONTHS)
    out += bytes([0])        # suppressDay: show the day name
    out += bytes([0xFF])     # lngDateFmt: day name, month, day of month, year
    out += bytes([0])        # dayLeading0: no
    out += bytes([3])        # abbrLen
    out += raw("", 4) + raw(", ", 4) + raw(" ", 4) + raw(", ", 4) + raw("", 4)  # st0-st4
    out += be16(0x0001)      # intl1Vers: region 0, version 1
    out += be16(0xA89F)      # localRtn: $A89F, so the extension follows
    assert len(out) == 332
    tables = [
        string_table([]),                       # extra days
        string_table([]),                       # extra months
        string_table([d[:3] for d in DAYS]),    # abbreviated days
        string_table([m[:3] for m in MONTHS]),  # abbreviated months
        string_table(["-", "."]),               # extra separators
    ]
    head = be16(0x0700) + be16(1) + be16(0)     # version, format, calendarCode: Gregorian
    at = len(out) + len(head) + 8 * len(tables)
    for t in tables:
        head += be32(at) + be32(len(t))
        at += len(t)
    return out + head + b"".join(tables)


# ---------------------------------------------------------------------------
# Break tables (NBreakTable) for FindWordBreaks

def state_table(rows, nclasses):
    """A state table: row offsets (state 0, the exit, has none), then one
    row of action codes per state.  rows[s][k] is (mark, next state)."""
    n = len(rows) + 1
    out = bytearray(be16(0))
    for s in range(1, n):
        out += be16(2 * n + (s - 1) * nclasses)
    for row in rows:
        assert len(row) == nclasses
        out += bytes((0x80 if mark else 0) | nxt * 2 for mark, nxt in row)
    return bytes(out)


def break_table(classes, backward, forward, nclasses, do_backup=6):
    back = pad(state_table(backward, nclasses))
    fwd = state_table(forward, nclasses)
    head_len, aux = 16, be16(0)    # one-byte characters only: no 2-byte classes
    back_off = head_len + 256 + len(aux)
    fwd_off = back_off + len(back)
    body = pad(bytes(classes) + aux + back + fwd)
    length = head_len + len(body)
    head = bytes([0x80, 0x00]) + be16(0) + be16(head_len) + be16(head_len + 256) + be16(back_off)
    head += be16(fwd_off) + be16(do_backup) + be16(length)
    return head + body


def safe_start_backward(cls, nclasses, blank, cr, nonbreak):
    """The backward table: from the selected character back to an offset
    that is sure to be a boundary for the forward machine, a character
    right after a return, or a character other than white space right
    after a blank.  Characters are marked as they are passed, and the
    machine stops, keeping the last mark, at the blank or return before
    one of those."""
    S, W = 2, 3   # the marked character starts a word (S) or is white (W)
    def kind(k):
        return "cr" if k == cr else "white" if k in (blank, nonbreak) else "text"
    start = [(True, S if kind(k) == "text" else W) for k in range(nclasses)]
    safe = []
    for k in range(nclasses):
        safe.append({"text": (True, S), "white": (False, 0) if k == blank else (True, W),
                     "cr": (False, 0)}[kind(k)])
    white = [{"text": (True, S), "white": (True, W), "cr": (False, 0)}[kind(k)] for k in range(nclasses)]
    return [start, safe, white]


# Word selection classes (Inside Macintosh: Text, table B-6).
(W_BREAK, W_NONBREAK, W_LETTER, W_NUMBER, W_MIDLETTER, W_MIDLETNUM, W_PRENUM, W_POSTNUM, W_MIDNUM,
 W_PREMIDNUM, W_BLANK, W_CR) = range(12)


def word_class(c):
    ch = ROMAN[c]
    if c == 0x0D:
        return W_CR
    if c in BLANKS:
        return W_BLANK
    if c == 0xCA:
        return W_NONBREAK
    if is_letter(c) or c in ACCENTS:
        return W_LETTER
    if 0x30 <= c <= 0x39:
        return W_NUMBER
    if ch == "-":
        return W_MIDLETTER
    if ch in "'’":
        return W_MIDLETNUM
    if ch in "%‰¢":
        return W_POSTNUM
    if unicodedata.category(ch) == "Sc":
        return W_PRENUM
    if ch in ",⁄":
        return W_MIDNUM
    if ch == ".":
        return W_PREMIDNUM
    return W_BREAK


def word_forward():
    """The forward word-selection machine, with the states of table B-7:
    1 start (or a leading nonbreaking space), 2 letters, 3 digits, 4 a
    character standing alone, 5 a prefix of a number ($ or .), 6 blanks,
    7 letters then a mid-word character, 8 a nonbreaking space after a
    word, 9 digits then a mid-number character, 10 a return, 11 blanks then
    a nonbreaking space."""
    n = 12
    def row(default, **k):
        r = [default] * n
        for cls, act in k.items():
            r[globals()["W_" + cls.upper()]] = act
        return r
    X, NX = (True, 0), (False, 0)     # exit here; exit at the last mark
    def m(s):
        return (True, s)
    word = dict(nonbreak=m(8), letter=m(2), number=m(3), midletter=m(4), midletnum=m(4), prenum=m(5),
                postnum=m(4), midnum=m(4), premidnum=m(5))
    return [
        row(m(4), **{**word, "nonbreak": m(1), "blank": m(6), "cr": m(10)}),                     # 1
        row(X, letter=m(2), number=m(3), midletter=m(7), midletnum=m(7), premidnum=m(7), nonbreak=m(8)),  # 2
        row(X, letter=m(2), number=m(3), midletnum=m(9), midnum=m(9), premidnum=m(9), postnum=m(4),
            nonbreak=m(8)),                                                                    # 3
        row(X, nonbreak=m(8)),                                                                 # 4
        row(X, number=m(3), nonbreak=m(8)),                                                    # 5
        row(X, blank=m(6), cr=m(10), nonbreak=m(11)),                                          # 6
        row(NX, letter=m(2)),                                                                  # 7
        row(m(4), **{**word, "blank": X, "cr": X}),                                            # 8
        row(NX, number=m(3)),                                                                  # 9
        row(X),                                                                                # 10
        row(NX, blank=m(6), cr=m(10), nonbreak=(False, 11)),                                   # 11
    ]


# Line break classes.
L_TEXT, L_NONBREAK, L_BLANK, L_CR, L_AFTER = range(5)
# A line may break after a hyphen, a dash, a slash or an operator.
BREAK_AFTER = "&*+-/<=>\\^|≠±≤≥≈–—÷"


def line_class(c):
    if c == 0x0D:
        return L_CR
    if c in BLANKS:
        return L_BLANK
    if c == 0xCA:
        return L_NONBREAK
    if ROMAN[c] in BREAK_AFTER:
        return L_AFTER
    return L_TEXT


def line_forward():
    """The forward line-break machine: 1 start (or leading nonbreaking
    spaces, which go with what follows), 2 text, 3 after a hyphen or
    operator (a run of them stays together), 4 blanks (with a return that
    ends them), 5 a return, 6 blanks then nonbreaking spaces (which go with
    the blanks before a blank, else with the text after)."""
    X, NX = (True, 0), (False, 0)
    def m(s):
        return (True, s)
    def row(text, nonbreak, blank, cr, after):
        return [text, nonbreak, blank, cr, after]
    return [
        row(m(2), m(1), m(4), m(5), m(3)),          # 1
        row(m(2), m(2), X, X, m(3)),                # 2
        row(X, m(2), X, X, m(3)),                   # 3
        row(X, m(6), m(4), m(5), X),                # 4
        row(X, X, X, X, X),                         # 5
        row(NX, (False, 6), m(4), m(5), NX),        # 6
    ]


# ---------------------------------------------------------------------------
# 'itl2': the string-manipulation resource, System 7 format

MOVEQ_0_D0, RTS = b"\x70\x00", b"\x4e\x75"


def itl2():
    # Classes: characters alike in type, case and marks share one, and each
    # list gives a class the value added to the character code.
    keys, classes, cls_of = [], {}, []
    for c in range(256):
        k = (char_type(c),) + tuple((f(c) - c) & 0xFF for f in
                                    (to_upper, to_lower, lambda x: to_upper(strip_mark(x)), strip_mark))
        if k not in classes:
            classes[k] = len(keys)
            keys.append(k)
        cls_of.append(classes[k])
    lists = [pad(bytes(k[i] for k in keys)) for i in range(5)]
    words = break_table([word_class(c) for c in range(256)],
                        safe_start_backward(word_class, 12, W_BLANK, W_CR, W_NONBREAK), word_forward(), 12)
    lines = break_table([line_class(c) for c in range(256)],
                        safe_start_backward(line_class, 5, L_BLANK, L_CR, L_NONBREAK), line_forward(), 5)
    # Empty sorting hooks, so the built-in U.S. order stands (as in Apple's
    # U.S. resource): RTS, and MOVEQ #0,D0; RTS for vernier and project.
    hooks = [RTS, RTS, MOVEQ_0_D0 + RTS, MOVEQ_0_D0 + RTS, RTS]
    names = ["init", "fetch", "vernier", "project", "exit", "class", "type", "upper", "lower", "upstrip",
             "strip", "word", "line"]
    parts = hooks + [bytes(cls_of)] + lists + [words, lines]
    at, offs, lens = 64, {}, {}
    body = b""
    for name, data in zip(names, parts):
        offs[name], lens[name] = at, len(data)
        body += data
        at += len(data)
    # The header: offsets to the hooks (the -1 format flag where the
    # reserved hook was), type list, class array, upper, lower, upper-strip,
    # word, line-break and strip lists; version and format; their lengths
    # (the flag's slot, which Apple's U.S. resource uses for an extension of
    # its own, 0); the findScriptTable offset and length.
    slots = ["init", "fetch", "vernier", "project", None, "exit", "type", "class", "upper", "lower", "upstrip",
             "word", "line", "strip"]
    head = b"".join(be16(-1 if s is None else offs[s]) for s in slots)
    head += be16(0x0200) + be16(1)                 # version, format 1: the System 7 header
    head += b"".join(be16(0 if s is None else lens[s]) for s in slots)
    head += be16(0) + be16(0)                      # findScriptTable: none for Roman
    assert len(head) == 64
    return head + pad(body)


# ---------------------------------------------------------------------------
# 'itl4': tokens (NItl4Rec)

# The token codes of IntlTokenize, with the canonical strings of the fixed
# ones; None for tokens with no fixed text.
TOKENS = [
    None, " ", '"', '"', None, None, "\r", "/*", "*/", None,        # 0 unknown .. 9 literal
    "\\", None, None, None, None, None, "(", ")", "[", "]",         # 10 escape .. 19 right bracket
    "{", "}", "«", "»", "+", "-", "*", "÷", "±", "/",               # 20 left curly .. 29 slash
    "\\", "<", ">", "=", "<=", "≤", ">=", "≥", "==", ":=",          # 30 backslash .. 39 colon-equal
    "≠", "<>", "!=", "!", "~", ",", ".", "“", "”", "‘",             # 40 not equal .. 49 left single quote
    "’", '"', "'", ";", "%", "^", "_", "&", "@", "|",               # 50 right single quote .. 59 bar
    "?", "π", "√", "∑", "∫", "µ", "∏", "∞", ":", "#",               # 60 question .. 69 hash
    "$", "\u00a0", "⁄", "€", "‹", "›", "‰", "…", "•",               # 70 dollar .. 78 center dot
]
TOKEN_WHITE, TOKEN_ALPHA, TOKEN_NUMERIC, TOKEN_NEWLINE, TOKEN_CENTER_DOT = 1, 4, 5, 6, 78


def token_map():
    """Each character's token: letters alpha, digits numeric, the fixed
    punctuation its own token (the quote and backslash tokens rather than
    the literal and escape delimiters), anything else unknown."""
    single = {}
    for t, s in enumerate(TOKENS):
        if t >= 16 and s and len(s) == 1:
            single.setdefault(s, t)
    out = bytearray()
    for c in range(256):
        ch = ROMAN[c]
        if is_letter(c) or c in ACCENTS:
            out.append(TOKEN_ALPHA)
        elif 0x30 <= c <= 0x39:
            out.append(TOKEN_NUMERIC)
        elif ch in "\t ":
            out.append(TOKEN_WHITE)
        elif ch == "\r":
            out.append(TOKEN_NEWLINE)
        elif ch == "·":
            out.append(TOKEN_CENTER_DOT)
        else:
            out.append(single.get(ch, 0))
    return bytes(out)


def untoken_table():
    strings, index = b"", []
    base = 4 + 2 * len(TOKENS)
    for s in TOKENS:
        index.append(base + len(strings))
        strings += pad(pstr("??" if s is None else s))
    body = be16(len(TOKENS) - 1) + b"".join(be16(i) for i in index) + strings
    return be16(2 + len(body)) + body


def wide_array(chars):
    """WideCharArr: the count less one, then ten wide characters."""
    return be16(len(chars) - 1) + b"".join(be16(ord(c)) for c in chars).ljust(20, b"\0")


def number_parts():
    """NumberParts for U.S. numbers."""
    # 1-14: quotes, leader placer, leader (the nonbreaking space), no-leader
    # and zero-leader markers, percent, plus, minus, thousands, (reserved),
    # list separator, escape, decimal point; then the unquoted literals.
    data = [ord(c) for c in "''^"] + [0xCA] + [ord(c) for c in "#0%+-,\0;\\.()"]
    out = be16(1) + b"".join(be16(v) for v in data).ljust(62, b"\0")
    out += wide_array("E+") + wide_array("E-") + wide_array("E") + wide_array("0123456789")
    return out + bytes(20)


def white_space():
    chars = [" ", "\t"]
    at = 4 + 2 * len(chars)
    offs, strings = [], b""
    for ch in chars:
        offs.append(at + len(strings))
        strings += pstr(ch)
    body = be16(len(chars)) + b"".join(be16(o) for o in offs) + strings
    return pad(be16(2 + len(body)) + body)


def itl4():
    head_len = 0x44
    # The string-copy routine and the extension fetcher (for 2-byte
    # characters, which Roman has none of) do nothing here: IntlTokenize is
    # native.
    parts = [("map", token_map()), ("str", RTS), ("fetch", RTS), ("untoken", untoken_table()),
             ("parts", number_parts()), ("white", white_space())]
    at, offs, body = head_len, {}, b""
    for name, data in parts:
        offs[name] = (at, len(data))
        body += data
        at += len(data)
    head = be16(0) + b"itl4" + be16(0) + be16(0x0700) + be16(1) + be16(0) + be32(0) + be16(8)
    for name in ("map", "str", "fetch", "untoken", "parts", "white"):
        head += be32(offs[name][0])
    head += be32(0) + be32(0)
    head += be16(0) + be16(0) + be16(0)   # resLength1-3
    for name in ("untoken", "parts", "white"):
        head += be16(offs[name][1])
    head += be16(0) + be16(0)
    assert len(head) == head_len
    return head + body


# ---------------------------------------------------------------------------
# 'itlb' and 'itlc'

def itlb():
    """ItlbExtRec for the Roman script: resources 0, the U.S. keyboard, and
    the fonts the Script Manager reports (scriptmgr.c)."""
    flags = 1 << 0 | 1 << 1 | 1 << 2 | 1 << 8    # smsfIntellCP, SingByte, NatCase, SynchUnstyledTE
    out = be16(0) + be16(0) + be16(0) + be16(flags) + be16(0) + be16(0)  # itl0, itl1, itl2, flags, itl4, itl5
    out += be16(0)                    # itlbLang: English
    out += bytes([0, 0])              # itlbNumRep, itlbDateRep: western digits, Gregorian
    out += be16(0) + be16(0)          # itlbKeys: KCHR 0; itlbIcon
    out += be32(0)                    # itlbLocalSize: no script-local record
    for fond, size in ((4, 9), (3, 12), (3, 9), (0, 12), (3, 12), (3, 9)):  # mono, pref, small, sys, app, help
        out += be16(fond) + be16(size)
    out += bytes([0x7F, 0x02])        # every style valid; aliases in italic
    assert len(out) == 50
    return out


def itlc():
    out = be16(0) + be16(0)           # itlcSystem: Roman; reserved
    out += bytes([0, 0xFF, 0, 0])     # font force off, international resources forced, old keyboard, flags
    out += be16(0) + bytes([0, 0])    # icon offset, side (reserved)
    out += be16(0)                    # itlcRegionCode: U.S.
    out += be16(0)                    # itlcSysFlags: left to right
    out += bytes(32)
    assert len(out) == 48
    return out


def resources():
    """(type, id, label, data) for each resource."""
    return [
        ("itl0", 0, "U.S. number, date and time formats", itl0()),
        ("itl1", 0, "U.S. long dates", itl1()),
        ("itl2", 0, "U.S. string manipulation", itl2()),
        ("itl4", 0, "U.S. tokens", itl4()),
        ("itlb", 0, "Roman script bundle", itlb()),
        ("itlc", 0, "international configuration", itlc()),
    ]
