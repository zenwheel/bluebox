#!/usr/bin/env python3
"""Generate Tests/PPC/altivec_vectors.txt: random and edge-case inputs for every
AltiVec register-to-register instruction, with the results of a reference
model written here from the AltiVec Technology Programming Environments
Manual's descriptions, separately from the interpreter (ppc_vec.c).  The
Swift test (AltiVecTests.swift) runs each case through the interpreter.

Each line: name op vA vB vC vscr -> vD vscr cr6, vectors as 32 hex digits
(element 0 first, big-endian), op, vscr and cr6 (CR field 6, 4 bits) in hex.
The choices the manual leaves open are the interpreter's (see ppc_vec.c):
estimates exact to single precision, NaN operands in vA, vB, vC order,
vctsxs/vctuxs give 0 for a NaN without SAT, vsl/vsr by byte 15's count.

    python3 -I Tests/PPC/gen_altivec.py
"""
import math
import os
import random
import struct
from fractions import Fraction

NJ, SAT = 0x10000, 1
DEFAULT_NAN = 0x7FC00000

# ---------------------------------------------------------------- vectors


def elems(v, size):
    """A 128-bit int as elements of `size` bytes, element 0 first."""
    n = 16 // size
    bits = size * 8
    return [(v >> (bits * (n - 1 - i))) & ((1 << bits) - 1) for i in range(n)]


def join(es, size):
    bits = size * 8
    v = 0
    for e in es:
        v = (v << bits) | (e & ((1 << bits) - 1))
    return v


def sx(x, bits):
    x &= (1 << bits) - 1
    return x - (1 << bits) if x >> (bits - 1) else x


class Sat:
    def __init__(self):
        self.hit = False

    def u(self, x, bits):
        hi = (1 << bits) - 1
        if x < 0:
            self.hit = True
            return 0
        if x > hi:
            self.hit = True
            return hi
        return x

    def s(self, x, bits):
        lo, hi = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
        if x < lo:
            self.hit = True
            x = lo
        elif x > hi:
            self.hit = True
            x = hi
        return x & ((1 << bits) - 1)


# ---------------------------------------------------------------- floats


def f_is_nan(u):
    return (u >> 23) & 0xFF == 0xFF and u & 0x7FFFFF != 0


def f_is_inf(u):
    return (u >> 23) & 0xFF == 0xFF and u & 0x7FFFFF == 0


def f_is_den(u):
    return (u >> 23) & 0xFF == 0 and u & 0x7FFFFF != 0


def f_is_zero(u):
    return u & 0x7FFFFFFF == 0


def f_val(u):
    """An ordinary (finite) float as an exact Fraction."""
    s = -1 if u >> 31 else 1
    e = (u >> 23) & 0xFF
    m = u & 0x7FFFFF
    if e == 0:
        return s * Fraction(m, 1 << 149)
    return s * Fraction(m | 0x800000) * Fraction(2) ** (e - 150)


def round_f32(q, neg_zero=False):
    """The exact value q rounded to nearest-even single precision, as bits."""
    if q == 0:
        return 0x80000000 if neg_zero else 0
    sign = 0x80000000 if q < 0 else 0
    q = abs(q)
    e = q.numerator.bit_length() - q.denominator.bit_length()
    if Fraction(2) ** e > q:
        e -= 1
    while Fraction(2) ** (e + 1) <= q:
        e += 1
    e = max(e, -126)
    scale = Fraction(2) ** (e - 23)
    m = q / scale
    mi = m.numerator // m.denominator
    rem = m - mi
    if rem > Fraction(1, 2) or (rem == Fraction(1, 2) and mi & 1):
        mi += 1
    if mi == 1 << 24:
        mi >>= 1
        e += 1
    if e > 127:
        return sign | 0x7F800000
    if mi < 1 << 23:  # denormal
        return sign | mi
    return sign | ((e + 127) << 23) | (mi & 0x7FFFFF)


def double_to_f32(d):
    """A Python float rounded to single, as bits (exact through Fraction)."""
    if math.isnan(d):
        return DEFAULT_NAN
    if math.isinf(d):
        return 0xFF800000 if d < 0 else 0x7F800000
    return round_f32(Fraction(d), neg_zero=math.copysign(1, d) < 0)


class FP:
    """The vector unit's float rules: NJ flushing, NaN choice."""

    def __init__(self, vscr):
        self.nj = bool(vscr & NJ)

    def inp(self, u):
        if self.nj and f_is_den(u):
            return u & 0x80000000
        return u

    def out(self, u):
        if self.nj and f_is_den(u):
            return u & 0x80000000
        return u

    @staticmethod
    def first_nan(*us):
        for u in us:
            if f_is_nan(u):
                return u | 0x400000
        return None


def to_float(u):
    return struct.unpack(">f", struct.pack(">I", u))[0]


def add_exact(a, b):
    """a + b of two float bit patterns, IEEE single, nearest-even."""
    if f_is_inf(a) or f_is_inf(b):
        if f_is_inf(a) and f_is_inf(b) and (a ^ b) >> 31:
            return DEFAULT_NAN
        return a if f_is_inf(a) else b
    if f_is_zero(a) and f_is_zero(b):
        return a & b & 0x80000000  # -0 only when both are -0
    q = f_val(a) + f_val(b)
    return round_f32(q, neg_zero=False)


def neg(u):
    return u ^ 0x80000000


def madd_exact(a, c, b):
    """a*c + b, rounded once."""
    prod_inf = f_is_inf(a) or f_is_inf(c)
    if (f_is_inf(a) and f_is_zero(c)) or (f_is_zero(a) and f_is_inf(c)):
        return DEFAULT_NAN
    psign = (a ^ c) & 0x80000000
    if prod_inf:
        if f_is_inf(b) and (b & 0x80000000) != psign:
            return DEFAULT_NAN
        return psign | 0x7F800000
    if f_is_inf(b):
        return b
    p = f_val(a) * f_val(c)
    if p == 0 and f_is_zero(b):
        # +0 unless both the product and b are -0
        return 0x80000000 if psign and b >> 31 else 0
    return round_f32(p + f_val(b))


def fp_op(name, a, b, uimm, vscr, sat):
    fp = FP(vscr)
    ua, ub = fp.inp(a), fp.inp(b)
    if name in ("vaddfp", "vsubfp", "vmaxfp", "vminfp"):
        n = FP.first_nan(ua, ub)
        if n is not None:
            return n
        if name == "vaddfp":
            return fp.out(add_exact(ua, ub))
        if name == "vsubfp":
            return fp.out(add_exact(ua, neg(ub)))
        fa, fb = to_float(ua), to_float(ub)
        if fa == fb:
            return (ua & ub) if name == "vmaxfp" else (ua | ub)
        big = ua if fa > fb else ub
        small = ub if fa > fb else ua
        return big if name == "vmaxfp" else small
    if name in ("vcfux", "vcfsx"):
        x = b if name == "vcfux" else sx(b, 32)
        return round_f32(Fraction(x, 1 << uimm))
    n = FP.first_nan(ub)
    if name in ("vctuxs", "vctsxs"):
        if n is not None:
            return 0
        if f_is_inf(ub):
            t = -(1 << 40) if neg(ub) >> 31 == 0 else 1 << 40
        else:
            t = math.trunc(f_val(ub) * (1 << uimm))
        return sat.u(t, 32) if name == "vctuxs" else sat.s(t, 32)
    if n is not None:
        return n
    neg_b = ub >> 31 == 1
    if name == "vrefp":
        if f_is_zero(ub):
            return (ub & 0x80000000) | 0x7F800000
        if f_is_inf(ub):
            return ub & 0x80000000
        return fp.out(round_f32(1 / f_val(ub)))
    if name == "vrsqrtefp":
        if f_is_zero(ub):
            return (ub & 0x80000000) | 0x7F800000
        if neg_b:
            return DEFAULT_NAN
        if f_is_inf(ub):
            return 0
        return fp.out(double_to_f32(1.0 / math.sqrt(to_float(ub))))
    if name == "vexptefp":
        if f_is_inf(ub):
            return 0 if neg_b else 0x7F800000
        return fp.out(double_to_f32(math.exp2(to_float(ub)) if to_float(ub) < 1024 else math.inf))
    if name == "vlogefp":
        if f_is_zero(ub):
            return 0xFF800000
        if neg_b:
            return DEFAULT_NAN
        if f_is_inf(ub):
            return 0x7F800000
        return fp.out(double_to_f32(math.log2(to_float(ub))))
    if name in ("vrfin", "vrfiz", "vrfip", "vrfim"):
        if f_is_inf(ub) or f_is_zero(ub):
            return ub
        q = f_val(ub)
        if name == "vrfiz":
            r = math.trunc(q)
        elif name == "vrfip":
            r = math.ceil(q)
        elif name == "vrfim":
            r = math.floor(q)
        else:
            r = round(q)  # Fraction rounds half to even
        return fp.out(round_f32(Fraction(r), neg_zero=neg_b))
    raise ValueError(name)


# ---------------------------------------------------------------- the model


def rot(x, n, bits):
    n %= bits
    m = (1 << bits) - 1
    return ((x << n) | (x >> (bits - n))) & m if n else x


def model(name, A, B, C, vscr, uimm=0, simm=0, sh=0):
    """Returns (vD, vscr, cr6 or None)."""
    sat = Sat()
    cr6 = None
    size = {"b": 1, "h": 2, "w": 4}

    def ew(fn, sz, signed=False):
        bits = sz * 8
        ea, eb = elems(A, sz), elems(B, sz)
        if signed:
            ea = [sx(x, bits) for x in ea]
            eb = [sx(x, bits) for x in eb]
        return join([fn(x, y) for x, y in zip(ea, eb)], sz)

    def suffix_size(n):
        return size[n[-1]] if n[-1] in "bhw" else None

    D = None
    n = name
    # modulo add/sub
    if n in ("vaddubm", "vadduhm", "vadduwm"):
        D = ew(lambda x, y: x + y, size[n[-2]])
    elif n in ("vsububm", "vsubuhm", "vsubuwm"):
        D = ew(lambda x, y: x - y, size[n[-2]])
    elif n == "vaddcuw":
        D = ew(lambda x, y: (x + y) >> 32, 4)
    elif n == "vsubcuw":
        D = ew(lambda x, y: 1 if x >= y else 0, 4)
    elif n in ("vaddubs", "vadduhs", "vadduws"):
        sz = size[n[-2]]
        D = ew(lambda x, y: sat.u(x + y, sz * 8), sz)
    elif n in ("vsububs", "vsubuhs", "vsubuws"):
        sz = size[n[-2]]
        D = ew(lambda x, y: sat.u(x - y, sz * 8), sz)
    elif n in ("vaddsbs", "vaddshs", "vaddsws"):
        sz = size[n[-2]]
        D = ew(lambda x, y: sat.s(x + y, sz * 8), sz, True)
    elif n in ("vsubsbs", "vsubshs", "vsubsws"):
        sz = size[n[-2]]
        D = ew(lambda x, y: sat.s(x - y, sz * 8), sz, True)
    elif n[:4] in ("vmax", "vmin") and n[-2:] != "fp":
        sz = size[n[-1]]
        f = max if n[:4] == "vmax" else min
        D = ew(f, sz, n[-2] == "s")
    elif n.startswith("vavg"):
        sz = size[n[-1]]
        D = ew(lambda x, y: (x + y + 1) >> 1, sz, n[-2] == "s")
    elif n in ("vand", "vandc", "vor", "vnor", "vxor"):
        f = {"vand": lambda: A & B, "vandc": lambda: A & ~B, "vor": lambda: A | B,
             "vnor": lambda: ~(A | B), "vxor": lambda: A ^ B}[n]
        D = f() & ((1 << 128) - 1)
    elif n in ("vrlb", "vrlh", "vrlw"):
        sz = size[n[-1]]
        D = ew(lambda x, y: rot(x, y % (sz * 8), sz * 8), sz)
    elif n in ("vslb", "vslh", "vslw"):
        sz = size[n[-1]]
        D = ew(lambda x, y: x << (y % (sz * 8)), sz)
    elif n in ("vsrb", "vsrh", "vsrw"):
        sz = size[n[-1]]
        D = ew(lambda x, y: x >> (y % (sz * 8)), sz)
    elif n in ("vsrab", "vsrah", "vsraw"):
        sz = size[n[-1]]
        bits = sz * 8
        D = join([sx(x, bits) >> (y % bits) for x, y in zip(elems(A, sz), elems(B, sz))], sz)
    elif n == "vsl":
        D = (A << (B & 7)) & ((1 << 128) - 1)
    elif n == "vsr":
        D = A >> (B & 7)
    elif n == "vslo":
        D = (A << (8 * ((B >> 3) & 15))) & ((1 << 128) - 1)
    elif n == "vsro":
        D = A >> (8 * ((B >> 3) & 15))
    elif n[:5] in ("vmrgh", "vmrgl"):
        sz = size[n[-1]]
        cnt = 16 // sz
        ea, eb = elems(A, sz), elems(B, sz)
        half = range(0, cnt // 2) if n[4] == "h" else range(cnt // 2, cnt)
        out = []
        for i in half:
            out += [ea[i], eb[i]]
        D = join(out, sz)
    elif n in ("vspltb", "vsplth", "vspltw"):
        sz = size[n[-1]]
        e = elems(B, sz)[uimm % (16 // sz)]
        D = join([e] * (16 // sz), sz)
    elif n in ("vspltisb", "vspltish", "vspltisw"):
        sz = size[n[-1]]
        D = join([simm] * (16 // sz), sz)
    elif n == "vperm":
        ab = elems(A, 1) + elems(B, 1)
        D = join([ab[x & 31] for x in elems(C, 1)], 1)
    elif n == "vsel":
        D = (A & ~C) | (B & C)
    elif n == "vsldoi":
        ab = elems(A, 1) + elems(B, 1)
        D = join(ab[sh:sh + 16], 1)
    elif n.startswith("vpk") and n != "vpkpx":
        src = 2 if n[4] == "h" else 4
        dst = src // 2
        es = elems(A, src) + elems(B, src)
        mode = n[3:]  # e.g. uhum, uhus, shus, shss
        out = []
        for x in es:
            if mode[0] == "s":
                x = sx(x, src * 8)
            if mode[2:] == "um":
                out.append(x & ((1 << (dst * 8)) - 1))
            elif mode[2:] == "us":
                out.append(sat.u(x, dst * 8))
            else:
                out.append(sat.s(x, dst * 8))
        D = join(out, dst)
    elif n == "vpkpx":
        out = []
        for w in elems(A, 4) + elems(B, 4):
            b = [(w >> 24) & 0xFF, (w >> 16) & 0xFF, (w >> 8) & 0xFF, w & 0xFF]
            out.append((b[0] & 1) << 15 | (b[1] >> 3) << 10 | (b[2] >> 3) << 5 | (b[3] >> 3))
        D = join(out, 2)
    elif n in ("vupkhsb", "vupklsb", "vupkhsh", "vupklsh"):
        sz = size[n[-1]]
        es = elems(B, sz)
        part = es[:len(es) // 2] if n[4] == "h" else es[len(es) // 2:]
        D = join([sx(x, sz * 8) for x in part], sz * 2)
    elif n in ("vupkhpx", "vupklpx"):
        es = elems(B, 2)
        part = es[:4] if n[4] == "h" else es[4:]
        D = join([(0xFF if h >> 15 else 0) << 24 | ((h >> 10) & 31) << 16 | ((h >> 5) & 31) << 8 | (h & 31)
                  for h in part], 4)
    elif n[:5] in ("vmule", "vmulo"):
        sz = size[n[-1]]
        bits = sz * 8
        ea, eb = elems(A, sz), elems(B, sz)
        if n[5] == "s":
            ea = [sx(x, bits) for x in ea]
            eb = [sx(x, bits) for x in eb]
        start = 0 if n[4] == "e" else 1
        D = join([ea[i] * eb[i] for i in range(start, 16 // sz, 2)], sz * 2)
    elif n in ("vmhaddshs", "vmhraddshs"):
        ea, eb, ec = ([sx(x, 16) for x in elems(V, 2)] for V in (A, B, C))
        rnd = 0x4000 if n == "vmhraddshs" else 0
        D = join([sat.s(((x * y + rnd) >> 15) + z, 16) for x, y, z in zip(ea, eb, ec)], 2)
    elif n == "vmladduhm":
        D = join([x * y + z for x, y, z in zip(elems(A, 2), elems(B, 2), elems(C, 2))], 2)
    elif n in ("vmsumubm", "vmsummbm"):
        ea, eb, ec = elems(A, 1), elems(B, 1), elems(C, 4)
        if n == "vmsummbm":
            ea = [sx(x, 8) for x in ea]
        D = join([sum(ea[4 * i + j] * eb[4 * i + j] for j in range(4)) + ec[i] for i in range(4)], 4)
    elif n in ("vmsumuhm", "vmsumuhs", "vmsumshm", "vmsumshs"):
        ea, eb, ec = elems(A, 2), elems(B, 2), elems(C, 4)
        if n[5] == "s":
            ea = [sx(x, 16) for x in ea]
            eb = [sx(x, 16) for x in eb]
            ec = [sx(x, 32) for x in ec]
        sums = [ea[2 * i] * eb[2 * i] + ea[2 * i + 1] * eb[2 * i + 1] + ec[i] for i in range(4)]
        if n[-1] == "m":
            D = join(sums, 4)
        elif n[5] == "u":
            D = join([sat.u(s, 32) for s in sums], 4)
        else:
            D = join([sat.s(s, 32) for s in sums], 4)
    elif n == "vsum4ubs":
        ea, eb = elems(A, 1), elems(B, 4)
        D = join([sat.u(sum(ea[4 * i:4 * i + 4]) + eb[i], 32) for i in range(4)], 4)
    elif n == "vsum4sbs":
        ea, eb = [sx(x, 8) for x in elems(A, 1)], [sx(x, 32) for x in elems(B, 4)]
        D = join([sat.s(sum(ea[4 * i:4 * i + 4]) + eb[i], 32) for i in range(4)], 4)
    elif n == "vsum4shs":
        ea, eb = [sx(x, 16) for x in elems(A, 2)], [sx(x, 32) for x in elems(B, 4)]
        D = join([sat.s(ea[2 * i] + ea[2 * i + 1] + eb[i], 32) for i in range(4)], 4)
    elif n == "vsum2sws":
        ea, eb = [sx(x, 32) for x in elems(A, 4)], [sx(x, 32) for x in elems(B, 4)]
        D = join([0, sat.s(ea[0] + ea[1] + eb[1], 32), 0, sat.s(ea[2] + ea[3] + eb[3], 32)], 4)
    elif n == "vsumsws":
        ea, eb = [sx(x, 32) for x in elems(A, 4)], [sx(x, 32) for x in elems(B, 4)]
        D = join([0, 0, 0, sat.s(sum(ea) + eb[3], 32)], 4)
    elif n in ("vmaddfp", "vnmsubfp"):
        fp = FP(vscr)
        out = []
        for a, b, c in zip(elems(A, 4), elems(B, 4), elems(C, 4)):
            a, b, c = fp.inp(a), fp.inp(b), fp.inp(c)
            nn = FP.first_nan(a, b, c)
            if nn is not None:
                out.append(nn)
            elif n == "vmaddfp":
                out.append(fp.out(madd_exact(a, c, b)))
            else:
                r = madd_exact(a, c, neg(b))
                out.append(DEFAULT_NAN if f_is_nan(r) else fp.out(neg(r)))
        D = join(out, 4)
    elif n in FP_VX:
        D = join([fp_op(n, a, b, uimm, vscr, sat) for a, b in zip(elems(A, 4), elems(B, 4))], 4)
    elif n.startswith("vcmp"):
        rc = n.endswith(".")
        base = n.rstrip(".")
        if base.endswith("fp"):
            fp = FP(vscr)
            out = []
            for a, b in zip(elems(A, 4), elems(B, 4)):
                a, b = fp.inp(a), fp.inp(b)
                fa, fb = to_float(a), to_float(b)
                if base == "vcmpbfp":
                    le = (not math.isnan(fa) and not math.isnan(fb)) and fa <= fb
                    ge = (not math.isnan(fa) and not math.isnan(fb)) and fa >= -fb
                    out.append((0 if le else 0x80000000) | (0 if ge else 0x40000000))
                else:
                    t = {"vcmpeqfp": fa == fb, "vcmpgefp": fa >= fb, "vcmpgtfp": fa > fb}[base]
                    out.append(0xFFFFFFFF if t else 0)
            D = join(out, 4)
            if rc:
                if base == "vcmpbfp":
                    cr6 = 0x2 if D == 0 else 0
        else:
            sz = size[base[-1]]
            bits = sz * 8
            ea, eb = elems(A, sz), elems(B, sz)
            if base[-2] == "s":
                ea = [sx(x, bits) for x in ea]
                eb = [sx(x, bits) for x in eb]
            t = [x == y if base[4:6] == "eq" else x > y for x, y in zip(ea, eb)]
            D = join([(1 << bits) - 1 if x else 0 for x in t], sz)
        if rc and cr6 is None:
            ones = (1 << 128) - 1
            cr6 = (0x8 if D == ones else 0) | (0x2 if D == 0 else 0)
    elif n == "mfvscr":
        D = vscr
    else:
        raise ValueError(n)
    D &= (1 << 128) - 1
    return D, vscr | (SAT if sat.hit else 0), cr6


FP_VX = {"vaddfp": 10, "vsubfp": 74, "vmaxfp": 1034, "vminfp": 1098, "vrefp": 266, "vrsqrtefp": 330,
         "vexptefp": 394, "vlogefp": 458, "vrfin": 522, "vrfiz": 586, "vrfip": 650, "vrfim": 714,
         "vcfux": 778, "vcfsx": 842, "vctuxs": 906, "vctsxs": 970}

VX = {
    "vaddubm": 0, "vmaxub": 2, "vrlb": 4, "vmuloub": 8, "vmrghb": 12, "vpkuhum": 14,
    "vadduhm": 64, "vmaxuh": 66, "vrlh": 68, "vmulouh": 72, "vmrghh": 76, "vpkuwum": 78,
    "vadduwm": 128, "vmaxuw": 130, "vrlw": 132, "vmrghw": 140, "vpkuhus": 142, "vpkuwus": 206,
    "vmaxsb": 258, "vslb": 260, "vmulosb": 264, "vmrglb": 268, "vpkshus": 270,
    "vmaxsh": 322, "vslh": 324, "vmulosh": 328, "vmrglh": 332, "vpkswus": 334,
    "vaddcuw": 384, "vmaxsw": 386, "vslw": 388, "vmrglw": 396, "vpkshss": 398,
    "vsl": 452, "vpkswss": 462,
    "vaddubs": 512, "vminub": 514, "vsrb": 516, "vmuleub": 520, "vspltb": 524, "vupkhsb": 526,
    "vadduhs": 576, "vminuh": 578, "vsrh": 580, "vmuleuh": 584, "vsplth": 588, "vupkhsh": 590,
    "vadduws": 640, "vminuw": 642, "vsrw": 644, "vspltw": 652, "vupklsb": 654,
    "vsr": 708, "vupklsh": 718,
    "vaddsbs": 768, "vminsb": 770, "vsrab": 772, "vmulesb": 776, "vspltisb": 780, "vpkpx": 782,
    "vaddshs": 832, "vminsh": 834, "vsrah": 836, "vmulesh": 840, "vspltish": 844, "vupkhpx": 846,
    "vaddsws": 896, "vminsw": 898, "vsraw": 900, "vspltisw": 908, "vupklpx": 974,
    "vsububm": 1024, "vavgub": 1026, "vand": 1028, "vslo": 1036,
    "vsubuhm": 1088, "vavguh": 1090, "vandc": 1092, "vsro": 1100,
    "vsubuwm": 1152, "vavguw": 1154, "vor": 1156, "vxor": 1220,
    "vavgsb": 1282, "vnor": 1284, "vavgsh": 1346, "vsubcuw": 1408, "vavgsw": 1410,
    "vsububs": 1536, "mfvscr": 1540, "vsum4ubs": 1544,
    "vsubuhs": 1600, "vsum4shs": 1608, "vsubuws": 1664, "vsum2sws": 1672,
    "vsubsbs": 1792, "vsum4sbs": 1800, "vsubshs": 1856, "vsubsws": 1920, "vsumsws": 1928,
}
VX.update(FP_VX)
VA = {"vmhaddshs": 32, "vmhraddshs": 33, "vmladduhm": 34, "vmsumubm": 36, "vmsummbm": 37,
      "vmsumuhm": 38, "vmsumuhs": 39, "vmsumshm": 40, "vmsumshs": 41, "vsel": 42, "vperm": 43,
      "vsldoi": 44, "vmaddfp": 46, "vnmsubfp": 47}
VC = {"vcmpequb": 6, "vcmpequh": 70, "vcmpequw": 134, "vcmpeqfp": 198, "vcmpgefp": 454,
      "vcmpgtub": 518, "vcmpgtuh": 582, "vcmpgtuw": 646, "vcmpgtfp": 710, "vcmpgtsb": 774,
      "vcmpgtsh": 838, "vcmpgtsw": 902, "vcmpbfp": 966}

UIMM_OPS = {"vspltb", "vsplth", "vspltw", "vcfux", "vcfsx", "vctuxs", "vctsxs"}
SIMM_OPS = {"vspltisb", "vspltish", "vspltisw"}
FLOAT_OPS = set(FP_VX) | {"vmaddfp", "vnmsubfp", "vcmpeqfp", "vcmpgefp", "vcmpgtfp", "vcmpbfp"}

# Registers: vD 1, vA 2, vB 3, vC 4.
D_, A_, B_, C_ = 1, 2, 3, 4

# Interesting single floats.
SPECIAL_F = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000, 0xFF800000, 0x7FC00000,
             0xFFC00000, 0x7F800001, 0xFF812345, 0x00000001, 0x80000001, 0x007FFFFF, 0x807FFFFF,
             0x00800000, 0x80800000, 0x7F7FFFFF, 0xFF7FFFFF, 0x3F000000, 0xBF000000, 0x3FC00000,
             0xBFC00000, 0x40200000, 0x4B000000, 0x4B7FFFFF, 0x4F000000, 0xCF000000, 0x4F800000,
             0x5F000000, 0xCF000001, 0x3EAAAAAB, 0x42F6E979, 0xC2FE0000, 0xC3000000, 0x42FE0000,
             0x7E800000, 0x01000000]
SPECIAL_I = [0, 1, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
             0x00010001, 0x80808080, 0x7F7F7F7F, 0x01020304]


def rand_vec(rng, kind):
    if kind == "f" and rng.random() < 0.25:  # denormals and the smallest normals, to flush or keep
        return join([rng.getrandbits(23) | (rng.getrandbits(1) << 31) | (rng.choice([0, 0, 0, 1]) << 23)
                     for _ in range(4)], 4)
    if kind == "f":
        es = []
        for _ in range(4):
            r = rng.random()
            if r < 0.35:
                es.append(rng.choice(SPECIAL_F))
            elif r < 0.5:
                es.append(rng.getrandbits(32))
            elif r < 0.6:  # small denormals and near-denormals
                es.append(rng.getrandbits(23) | (rng.getrandbits(1) << 31) | (rng.randrange(0, 3) << 23))
            else:  # moderate magnitudes
                f = rng.uniform(-1000, 1000) * (10 ** rng.randrange(-6, 6))
                es.append(double_to_f32(f))
        return join(es, 4)
    r = rng.random()
    if r < 0.25:
        return join([rng.choice(SPECIAL_I) for _ in range(4)], 4)
    if r < 0.4:
        return join([rng.choice([0, 0x7F, 0x80, 0xFF, 1, 0xFE]) for _ in range(16)], 1)
    if r < 0.5:
        return join([rng.choice([0, 0x7FFF, 0x8000, 0xFFFF, 1, 0x7FFE, 0x8001]) for _ in range(8)], 2)
    return rng.getrandbits(128)


def encode(name, uimm, simm, sh, rc=False):
    if name in VA:
        c = sh if name == "vsldoi" else C_
        return 4 << 26 | D_ << 21 | A_ << 16 | B_ << 11 | c << 6 | VA[name]
    if name.rstrip(".") in VC:
        return 4 << 26 | D_ << 21 | A_ << 16 | B_ << 11 | (0x400 if rc else 0) | VC[name.rstrip(".")]
    a = A_
    if name in UIMM_OPS:
        a = uimm
    elif name in SIMM_OPS:
        a = simm & 31
    elif name in ("vrefp", "vrsqrtefp", "vexptefp", "vlogefp", "vrfin", "vrfiz", "vrfip", "vrfim",
                  "vupkhsb", "vupkhsh", "vupklsb", "vupklsh", "vupkhpx", "vupklpx"):
        a = 0
    b = 0 if name in SIMM_OPS or name == "mfvscr" else B_
    if name == "mfvscr":
        a = 0
    return 4 << 26 | D_ << 21 | a << 16 | b << 11 | VX[name]


def main():
    rng = random.Random(0xA17EC)
    names = list(VX) + list(VA) + list(VC) + [n + "." for n in VC]
    lines = []
    for name in names:
        base = name.rstrip(".")
        kind = "f" if base in FLOAT_OPS and base not in ("vcfux", "vcfsx") else "i"
        count = 32 if kind == "f" else 12
        for k in range(count):
            A, B, C = rand_vec(rng, kind), rand_vec(rng, kind), rand_vec(rng, kind)
            if base == "vperm":
                C = rng.getrandbits(128)
            if base in ("vsl", "vsr"):  # the count the same in every byte, as the manual requires
                n = rng.randrange(8)
                B = join([(rng.getrandbits(5) << 3) | n for _ in range(16)], 1)
            uimm = rng.randrange(32)
            simm = rng.randrange(-16, 16)
            sh = rng.randrange(16)
            vscr = rng.choice([0, NJ, SAT, NJ | SAT]) if k % 3 else (NJ if k % 2 else 0)
            if base == "mfvscr":
                vscr = rng.choice([0, NJ, SAT, NJ | SAT])
            D, vscr_out, cr6 = model(name, A, B, C, vscr, uimm, simm, sh)
            op = encode(name, uimm, simm, sh, rc=name.endswith("."))
            lines.append("%s %08X %032X %032X %032X %X -> %032X %X %s" % (
                name, op, A, B, C, vscr, D, vscr_out, "-" if cr6 is None else "%X" % cr6))
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "altivec_vectors.txt")
    with open(out, "w") as f:
        f.write("# Generated by Tests/PPC/gen_altivec.py; see there.\n")
        f.write("\n".join(lines) + "\n")
    print("%d cases, %d instructions" % (len(lines), len(names)))


if __name__ == "__main__":
    main()
