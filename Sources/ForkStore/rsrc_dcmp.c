/*
 * Decompression of System 7 compressed resources ('dcmp' 0, 1, 2 and 3).
 *
 * The formats and lookup tables follow dgelessus's rsrcfork
 * (https://github.com/dgelessus/python-rsrcfork), MIT License,
 * Copyright (c) 2016 dgelessus.
 *
 * Header (18 bytes): 'a89f6572' signature, header length (0x12), type
 * (0x0801 = type 8, used by dcmp 0/1; 0x0901 = type 9, used by dcmp 2),
 * decompressed length, then 6 type-specific bytes.
 */
#include "forks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

/* dcmp 0: fixed table for codes 0x4b..0xfd. */
static const uint8_t dcmp0_table[358] = {
    0x00, 0x00, 0x4E, 0xBA, 0x00, 0x08, 0x4E, 0x75, 0x00, 0x0C, 0x4E, 0xAD, 0x20, 0x53, 0x2F, 0x0B,
    0x61, 0x00, 0x00, 0x10, 0x70, 0x00, 0x2F, 0x00, 0x48, 0x6E, 0x20, 0x50, 0x20, 0x6E, 0x2F, 0x2E,
    0xFF, 0xFC, 0x48, 0xE7, 0x3F, 0x3C, 0x00, 0x04, 0xFF, 0xF8, 0x2F, 0x0C, 0x20, 0x06, 0x4E, 0xED,
    0x4E, 0x56, 0x20, 0x68, 0x4E, 0x5E, 0x00, 0x01, 0x58, 0x8F, 0x4F, 0xEF, 0x00, 0x02, 0x00, 0x18,
    0x60, 0x00, 0xFF, 0xFF, 0x50, 0x8F, 0x4E, 0x90, 0x00, 0x06, 0x26, 0x6E, 0x00, 0x14, 0xFF, 0xF4,
    0x4C, 0xEE, 0x00, 0x0A, 0x00, 0x0E, 0x41, 0xEE, 0x4C, 0xDF, 0x48, 0xC0, 0xFF, 0xF0, 0x2D, 0x40,
    0x00, 0x12, 0x30, 0x2E, 0x70, 0x01, 0x2F, 0x28, 0x20, 0x54, 0x67, 0x00, 0x00, 0x20, 0x00, 0x1C,
    0x20, 0x5F, 0x18, 0x00, 0x26, 0x6F, 0x48, 0x78, 0x00, 0x16, 0x41, 0xFA, 0x30, 0x3C, 0x28, 0x40,
    0x72, 0x00, 0x28, 0x6E, 0x20, 0x0C, 0x66, 0x00, 0x20, 0x6B, 0x2F, 0x07, 0x55, 0x8F, 0x00, 0x28,
    0xFF, 0xFE, 0xFF, 0xEC, 0x22, 0xD8, 0x20, 0x0B, 0x00, 0x0F, 0x59, 0x8F, 0x2F, 0x3C, 0xFF, 0x00,
    0x01, 0x18, 0x81, 0xE1, 0x4A, 0x00, 0x4E, 0xB0, 0xFF, 0xE8, 0x48, 0xC7, 0x00, 0x03, 0x00, 0x22,
    0x00, 0x07, 0x00, 0x1A, 0x67, 0x06, 0x67, 0x08, 0x4E, 0xF9, 0x00, 0x24, 0x20, 0x78, 0x08, 0x00,
    0x66, 0x04, 0x00, 0x2A, 0x4E, 0xD0, 0x30, 0x28, 0x26, 0x5F, 0x67, 0x04, 0x00, 0x30, 0x43, 0xEE,
    0x3F, 0x00, 0x20, 0x1F, 0x00, 0x1E, 0xFF, 0xF6, 0x20, 0x2E, 0x42, 0xA7, 0x20, 0x07, 0xFF, 0xFA,
    0x60, 0x02, 0x3D, 0x40, 0x0C, 0x40, 0x66, 0x06, 0x00, 0x26, 0x2D, 0x48, 0x2F, 0x01, 0x70, 0xFF,
    0x60, 0x04, 0x18, 0x80, 0x4A, 0x40, 0x00, 0x40, 0x00, 0x2C, 0x2F, 0x08, 0x00, 0x11, 0xFF, 0xE4,
    0x21, 0x40, 0x26, 0x40, 0xFF, 0xF2, 0x42, 0x6E, 0x4E, 0xB9, 0x3D, 0x7C, 0x00, 0x38, 0x00, 0x0D,
    0x60, 0x06, 0x42, 0x2E, 0x20, 0x3C, 0x67, 0x0C, 0x2D, 0x68, 0x66, 0x08, 0x4A, 0x2E, 0x4A, 0xAE,
    0x00, 0x2E, 0x48, 0x40, 0x22, 0x5F, 0x22, 0x00, 0x67, 0x0A, 0x30, 0x07, 0x42, 0x67, 0x00, 0x32,
    0x20, 0x28, 0x00, 0x09, 0x48, 0x7A, 0x02, 0x00, 0x2F, 0x2B, 0x00, 0x05, 0x22, 0x6E, 0x66, 0x02,
    0xE5, 0x80, 0x67, 0x0E, 0x66, 0x0A, 0x00, 0x50, 0x3E, 0x00, 0x66, 0x0C, 0x2E, 0x00, 0xFF, 0xEE,
    0x20, 0x6D, 0x20, 0x40, 0xFF, 0xE0, 0x53, 0x40, 0x60, 0x08, 0x04, 0x80, 0x00, 0x68, 0x0B, 0x7C,
    0x44, 0x00, 0x41, 0xE8, 0x48, 0x41,
};

/* dcmp 1: fixed table for codes 0xd5..0xfd. */
static const uint8_t dcmp1_table[82] = {
    0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x2E, 0x01, 0x3E, 0x01, 0x01, 0x01, 0x1E, 0x01,
    0xFF, 0xFF, 0x0E, 0x01, 0x31, 0x00, 0x11, 0x12, 0x01, 0x07, 0x33, 0x32, 0x12, 0x39, 0xED, 0x10,
    0x01, 0x27, 0x23, 0x22, 0x01, 0x37, 0x07, 0x06, 0x01, 0x17, 0x01, 0x23, 0x00, 0xFF, 0x00, 0x2F,
    0x07, 0x0E, 0xFD, 0x3C, 0x01, 0x35, 0x01, 0x15, 0x01, 0x02, 0x00, 0x07, 0x00, 0x3E, 0x05, 0xD5,
    0x02, 0x01, 0x06, 0x07, 0x07, 0x08, 0x30, 0x01, 0x01, 0x33, 0x00, 0x10, 0x17, 0x16, 0x37, 0x3E,
    0x36, 0x37,
};

/* dcmp 2: default table, 256 entries. */
static const uint8_t dcmp2_table[512] = {
    0x00, 0x00, 0x00, 0x08, 0x4E, 0xBA, 0x20, 0x6E, 0x4E, 0x75, 0x00, 0x0C, 0x00, 0x04, 0x70, 0x00,
    0x00, 0x10, 0x00, 0x02, 0x48, 0x6E, 0xFF, 0xFC, 0x60, 0x00, 0x00, 0x01, 0x48, 0xE7, 0x2F, 0x2E,
    0x4E, 0x56, 0x00, 0x06, 0x4E, 0x5E, 0x2F, 0x00, 0x61, 0x00, 0xFF, 0xF8, 0x2F, 0x0B, 0xFF, 0xFF,
    0x00, 0x14, 0x00, 0x0A, 0x00, 0x18, 0x20, 0x5F, 0x00, 0x0E, 0x20, 0x50, 0x3F, 0x3C, 0xFF, 0xF4,
    0x4C, 0xEE, 0x30, 0x2E, 0x67, 0x00, 0x4C, 0xDF, 0x26, 0x6E, 0x00, 0x12, 0x00, 0x1C, 0x42, 0x67,
    0xFF, 0xF0, 0x30, 0x3C, 0x2F, 0x0C, 0x00, 0x03, 0x4E, 0xD0, 0x00, 0x20, 0x70, 0x01, 0x00, 0x16,
    0x2D, 0x40, 0x48, 0xC0, 0x20, 0x78, 0x72, 0x00, 0x58, 0x8F, 0x66, 0x00, 0x4F, 0xEF, 0x42, 0xA7,
    0x67, 0x06, 0xFF, 0xFA, 0x55, 0x8F, 0x28, 0x6E, 0x3F, 0x00, 0xFF, 0xFE, 0x2F, 0x3C, 0x67, 0x04,
    0x59, 0x8F, 0x20, 0x6B, 0x00, 0x24, 0x20, 0x1F, 0x41, 0xFA, 0x81, 0xE1, 0x66, 0x04, 0x67, 0x08,
    0x00, 0x1A, 0x4E, 0xB9, 0x50, 0x8F, 0x20, 0x2E, 0x00, 0x07, 0x4E, 0xB0, 0xFF, 0xF2, 0x3D, 0x40,
    0x00, 0x1E, 0x20, 0x68, 0x66, 0x06, 0xFF, 0xF6, 0x4E, 0xF9, 0x08, 0x00, 0x0C, 0x40, 0x3D, 0x7C,
    0xFF, 0xEC, 0x00, 0x05, 0x20, 0x3C, 0xFF, 0xE8, 0xDE, 0xFC, 0x4A, 0x2E, 0x00, 0x30, 0x00, 0x28,
    0x2F, 0x08, 0x20, 0x0B, 0x60, 0x02, 0x42, 0x6E, 0x2D, 0x48, 0x20, 0x53, 0x20, 0x40, 0x18, 0x00,
    0x60, 0x04, 0x41, 0xEE, 0x2F, 0x28, 0x2F, 0x01, 0x67, 0x0A, 0x48, 0x40, 0x20, 0x07, 0x66, 0x08,
    0x01, 0x18, 0x2F, 0x07, 0x30, 0x28, 0x3F, 0x2E, 0x30, 0x2B, 0x22, 0x6E, 0x2F, 0x2B, 0x00, 0x2C,
    0x67, 0x0C, 0x22, 0x5F, 0x60, 0x06, 0x00, 0xFF, 0x30, 0x07, 0xFF, 0xEE, 0x53, 0x40, 0x00, 0x40,
    0xFF, 0xE4, 0x4A, 0x40, 0x66, 0x0A, 0x00, 0x0F, 0x4E, 0xAD, 0x70, 0xFF, 0x22, 0xD8, 0x48, 0x6B,
    0x00, 0x22, 0x20, 0x4B, 0x67, 0x0E, 0x4A, 0xAE, 0x4E, 0x90, 0xFF, 0xE0, 0xFF, 0xC0, 0x00, 0x2A,
    0x27, 0x40, 0x67, 0x02, 0x51, 0xC8, 0x02, 0xB6, 0x48, 0x7A, 0x22, 0x78, 0xB0, 0x6E, 0xFF, 0xE6,
    0x00, 0x09, 0x32, 0x2E, 0x3E, 0x00, 0x48, 0x41, 0xFF, 0xEA, 0x43, 0xEE, 0x4E, 0x71, 0x74, 0x00,
    0x2F, 0x2C, 0x20, 0x6C, 0x00, 0x3C, 0x00, 0x26, 0x00, 0x50, 0x18, 0x80, 0x30, 0x1F, 0x22, 0x00,
    0x66, 0x0C, 0xFF, 0xDA, 0x00, 0x38, 0x66, 0x02, 0x30, 0x2C, 0x20, 0x0C, 0x2D, 0x6E, 0x42, 0x40,
    0xFF, 0xE2, 0xA9, 0xF0, 0xFF, 0x00, 0x37, 0x7C, 0xE5, 0x80, 0xFF, 0xDC, 0x48, 0x68, 0x59, 0x4F,
    0x00, 0x34, 0x3E, 0x1F, 0x60, 0x08, 0x2F, 0x06, 0xFF, 0xDE, 0x60, 0x0A, 0x70, 0x02, 0x00, 0x32,
    0xFF, 0xCC, 0x00, 0x80, 0x22, 0x51, 0x10, 0x1F, 0x31, 0x7C, 0xA0, 0x29, 0xFF, 0xD8, 0x52, 0x40,
    0x01, 0x00, 0x67, 0x10, 0xA0, 0x23, 0xFF, 0xCE, 0xFF, 0xD4, 0x20, 0x06, 0x48, 0x78, 0x00, 0x2E,
    0x50, 0x4F, 0x43, 0xFA, 0x67, 0x12, 0x76, 0x00, 0x41, 0xE8, 0x4A, 0x6E, 0x20, 0xD9, 0x00, 0x5A,
    0x7F, 0xFF, 0x51, 0xCA, 0x00, 0x5C, 0x2E, 0x00, 0x02, 0x40, 0x48, 0xC7, 0x67, 0x14, 0x0C, 0x80,
    0x2E, 0x9F, 0xFF, 0xD6, 0x80, 0x00, 0x10, 0x00, 0x48, 0x42, 0x4A, 0x6B, 0xFF, 0xD2, 0x00, 0x48,
    0x4A, 0x47, 0x4E, 0xD1, 0x20, 0x6F, 0x00, 0x41, 0x60, 0x0C, 0x2A, 0x78, 0x42, 0x2E, 0x32, 0x00,
    0x65, 0x74, 0x67, 0x16, 0x00, 0x44, 0x48, 0x6D, 0x20, 0x08, 0x48, 0x6C, 0x0B, 0x7C, 0x26, 0x40,
    0x04, 0x00, 0x00, 0x68, 0x20, 0x6D, 0x00, 0x0D, 0x2A, 0x40, 0x00, 0x0B, 0x00, 0x3E, 0x02, 0x20,
};

_Static_assert(sizeof dcmp0_table == 2 * (0xfe - 0x4b), "dcmp0 table size");
_Static_assert(sizeof dcmp1_table == 2 * (0xfe - 0xd5), "dcmp1 table size");
_Static_assert(sizeof dcmp2_table == 2 * 256, "dcmp2 table size");

/* ---------------------------------------------------------------------- */

typedef struct {
    const uint8_t *p, *end;
    uint8_t *out;
    size_t pos, cap;
    bool err;
    /* Literals saved for back-references (pointers into the output). */
    size_t *lit_off;
    uint32_t *lit_len;
    size_t nlit, litcap;
} dstate;

static int rd(dstate *s)
{
    if (s->p >= s->end) {
        s->err = true;
        return 0;
    }
    return *s->p++;
}

static void emit(dstate *s, const uint8_t *src, size_t n)
{
    /* Output may run one byte past the declared size (odd lengths); allow it. */
    if (s->pos + n > s->cap + 1) {
        s->err = true;
        return;
    }
    size_t m = s->pos + n > s->cap ? s->cap - s->pos : n;
    memmove(s->out + s->pos, src, m);
    s->pos += n;
}

static void emit_literal(dstate *s, size_t n, bool store)
{
    if ((size_t)(s->end - s->p) < n) {
        s->err = true;
        return;
    }
    if (store) {
        if (s->nlit == s->litcap) {
            s->litcap = s->litcap ? s->litcap * 2 : 64;
            s->lit_off = realloc(s->lit_off, s->litcap * sizeof *s->lit_off);
            s->lit_len = realloc(s->lit_len, s->litcap * sizeof *s->lit_len);
            if (!s->lit_off || !s->lit_len) {
                s->err = true;
                return;
            }
        }
        s->lit_off[s->nlit] = s->pos;
        s->lit_len[s->nlit] = (uint32_t)n;
        s->nlit++;
    }
    emit(s, s->p, n);
    s->p += n;
}

static void emit_backref(dstate *s, size_t index)
{
    if (index >= s->nlit) {
        s->err = true;
        return;
    }
    size_t off = s->lit_off[index], n = s->lit_len[index];
    if (off + n > s->cap) { /* literal was the truncated tail; cannot happen mid-stream */
        s->err = true;
        return;
    }
    emit(s, s->out + off, n);
}

static int32_t rd_varint(dstate *s)
{
    int head = rd(s);
    if (head == 0xFF) {
        uint32_t v = 0;
        for (int i = 0; i < 4; i++)
            v = (v << 8) | (uint32_t)rd(s);
        return (int32_t)v;
    }
    if (head >= 0x80) {
        uint16_t v = (uint16_t)((((head - 0xC0) & 0xFF) << 8) | rd(s));
        return (int16_t)v;
    }
    return (int8_t)head;
}

static void emit16(dstate *s, uint32_t v)
{
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    emit(s, b, 2);
}
static void emit32(dstate *s, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    emit(s, b, 4);
}

static void dcmp0(dstate *s)
{
    for (;;) {
        if (s->err)
            return;
        int b = rd(s);
        if (s->err)
            return;
        if (b < 0x20) {
            size_t count2 = (b == 0x00 || b == 0x10) ? (size_t)rd(s) : (size_t)(b & 0xF);
            emit_literal(s, count2 * 2, b >= 0x10);
        } else if (b == 0x20 || b == 0x21) {
            emit_backref(s, 0x28 + (size_t)(((b - 0x20) << 8) | rd(s)));
        } else if (b == 0x22) {
            size_t hi = (size_t)rd(s);
            emit_backref(s, 0x28 + ((hi << 8) | (size_t)rd(s)));
        } else if (b < 0x4B) {
            emit_backref(s, (size_t)(b - 0x23));
        } else if (b < 0xFE) {
            emit(s, dcmp0_table + (b - 0x4B) * 2, 2);
        } else if (b == 0xFE) {
            int kind = rd(s);
            if (kind == 0x00) { /* segment loader jump table */
                uint16_t seg = (uint16_t)rd_varint(s);
                uint8_t tail[6] = { 0x3F, 0x3C, (uint8_t)(seg >> 8), (uint8_t)seg, 0xA9, 0xF0 };
                emit(s, tail, 6);
                int32_t count = rd_varint(s);
                if (count <= 0) {
                    s->err = true;
                    return;
                }
                uint16_t cur = (uint16_t)rd_varint(s);
                emit16(s, cur);
                emit(s, tail, 6);
                for (int32_t i = 1; i < count && !s->err; i++) {
                    cur = (uint16_t)(cur + (rd_varint(s) - 6));
                    emit16(s, cur);
                    emit(s, tail, 6);
                }
            } else if (kind == 0x02 || kind == 0x03) { /* repeat 1 or 2 bytes */
                uint32_t v = (uint32_t)rd_varint(s);
                int32_t count = rd_varint(s) + 1;
                if (count <= 0) {
                    s->err = true;
                    return;
                }
                for (int32_t i = 0; i < count && !s->err; i++) {
                    if (kind == 0x02) {
                        uint8_t c = (uint8_t)v;
                        emit(s, &c, 1);
                    } else {
                        emit16(s, v);
                    }
                }
            } else if (kind == 0x04) { /* delta-encoded 16-bit words */
                uint16_t cur = (uint16_t)rd_varint(s);
                emit16(s, cur);
                int32_t count = rd_varint(s);
                for (int32_t i = 0; i < count && !s->err; i++) {
                    cur = (uint16_t)(cur + (int8_t)rd(s));
                    emit16(s, cur);
                }
            } else if (kind == 0x06) { /* delta-encoded 32-bit longs */
                uint32_t cur = (uint32_t)rd_varint(s);
                emit32(s, cur);
                int32_t count = rd_varint(s);
                for (int32_t i = 0; i < count && !s->err; i++) {
                    cur += (uint32_t)rd_varint(s);
                    emit32(s, cur);
                }
            } else {
                s->err = true;
                return;
            }
        } else { /* 0xFF: end */
            return;
        }
    }
}

static void dcmp1(dstate *s)
{
    for (;;) {
        if (s->err)
            return;
        int b = rd(s);
        if (s->err)
            return;
        if (b < 0x20) {
            emit_literal(s, (size_t)(b & 0xF) + 1, b >= 0x10);
        } else if (b < 0xD0) {
            emit_backref(s, (size_t)(b - 0x20));
        } else if (b == 0xD0 || b == 0xD1) {
            emit_literal(s, (size_t)rd(s), b == 0xD1);
        } else if (b == 0xD2) {
            emit_backref(s, 0xB0 + (size_t)rd(s));
        } else if (b >= 0xD5 && b < 0xFE) {
            emit(s, dcmp1_table + (b - 0xD5) * 2, 2);
        } else if (b == 0xFE) {
            if (rd(s) != 0x02) {
                s->err = true;
                return;
            }
            uint8_t v = (uint8_t)rd_varint(s);
            int32_t count = rd_varint(s) + 1;
            for (int32_t i = 0; i < count && !s->err; i++)
                emit(s, &v, 1);
        } else if (b == 0xFF) {
            return;
        } else {
            s->err = true;
            return;
        }
    }
}

static void dcmp2(dstate *s, const uint8_t *params, uint32_t outlen)
{
    unsigned table_count = (unsigned)params[2] + 1;
    unsigned flags = params[3];
    uint8_t custom[512];
    const uint8_t *table = dcmp2_table;
    if (flags & ~3u) {
        s->err = true;
        return;
    }
    if (flags & 1) {
        if ((size_t)(s->end - s->p) < table_count * 2) {
            s->err = true;
            return;
        }
        memcpy(custom, s->p, table_count * 2);
        s->p += table_count * 2;
        table = custom;
    } else {
        table_count = 256;
    }
    bool tagged = flags & 2;
    while (s->p < s->end && !s->err) {
        if (s->end - s->p == 1 && (outlen & 1)) {
            emit(s, s->p, 1);
            s->p++;
            break;
        }
        if (!tagged) {
            unsigned idx = *s->p++;
            if (idx >= table_count) {
                s->err = true;
                return;
            }
            emit(s, table + idx * 2, 2);
            continue;
        }
        uint8_t tag = *s->p++;
        for (int bit = 7; bit >= 0 && s->p < s->end; bit--) {
            if (tag & (1u << bit)) {
                unsigned idx = *s->p++;
                if (idx >= table_count) {
                    s->err = true;
                    return;
                }
                emit(s, table + idx * 2, 2);
            } else {
                size_t n = s->end - s->p >= 2 ? 2 : 1;
                emit(s, s->p, n);
                s->p += n;
            }
        }
    }
}

/*
 * dcmp 3 (System 7.5.3 and later; header type 9, id 3): an LZ77 coder over
 * an MSB-first bit stream.  A token is a length code and, for a match, a
 * distance code; length 0 means a run of literal bytes (also read from the
 * bit stream).  After a literal run shorter than 63 bytes the next token has
 * to be a match, so its length code counts from 3; otherwise a match counts
 * from 2 and a zero length means literals.  The distance code depends on how
 * much has been written: fifteen classes by output position, each with a
 * short form for near distances, a mid form, and a last form whose field is
 * just wide enough to reach back to the start of the output.
 */
typedef struct {
    int n0, n1;          /* bits of the near ("0") and mid ("10") forms */
    uint32_t maxpos;     /* the class covers positions up to this */
    unsigned nthr;
    const uint32_t *thr; /* the "11" form: n+1 bits while pos <= thr[n] */
} dcmp3_class;

static const uint32_t d3t0[] = { 7, 9, 13 };
static const uint32_t d3t1[] = { 12, 14, 18, 26 };
static const uint32_t d3t2[] = { 22, 24, 28, 36, 52 };
static const uint32_t d3t3[] = { 42, 44, 48, 56, 72, 104 };
static const uint32_t d3t4[] = { 82, 84, 88, 96, 112, 144, 208 };
static const uint32_t d3t5[] = { 162, 164, 168, 176, 192, 224, 288, 416 };
static const uint32_t d3t6[] = { 322, 324, 328, 336, 352, 384, 448, 576, 832 };
/* The last rung here is 1644, not 1664, as Apple's table has it. */
static const uint32_t d3t7[] = { 642, 644, 648, 656, 672, 704, 768, 896, 1152, 1644 };
static const uint32_t d3t8[] = { 1282, 1284, 1288, 1296, 1312, 1344, 1408, 1536, 1792, 2304, 3328 };
static const uint32_t d3t9[] = { 2562, 2564, 2568, 2576, 2592, 2624, 2688, 2816, 3072, 3584, 4608, 6656 };
static const uint32_t d3t10[] = { 5122, 5124, 5128, 5136, 5152, 5184, 5248, 5376, 5632, 6144, 7168, 9216, 13312 };
static const uint32_t d3t11[] = { 10242, 10244, 10248, 10256, 10272, 10304, 10368, 10496, 10752, 11264, 12288, 14336, 18432, 26624 };
static const uint32_t d3t12[] = { 20482, 20484, 20488, 20496, 20512, 20544, 20608, 20736, 20992, 21504, 22528, 24576, 28672, 36864, 53248 };
static const uint32_t d3t13[] = { 40962, 40964, 40968, 40976, 40992, 41024, 41088, 41216, 41472, 41984, 43008, 45056, 49152, 57344, 73728, 106496 };
/* Apple's seventh rung is 0x200C where 82048 was meant; it never holds, so
 * positions up to 82048 get the eighth rung's 8 bits.  Streams were written
 * to match, so it stays. */
static const uint32_t d3t14[] = { 81922, 81924, 81928, 81936, 81952, 81984, 0x200C, 82176, 82432, 82944, 83968, 86016, 90112, 98304, 114688, 147456, 212992 };

#define C(n0, n1, maxpos, t) { n0, n1, maxpos, sizeof t / sizeof t[0], t }
static const dcmp3_class dcmp3_classes[15] = {
    C(0, 2, 10, d3t0),       C(1, 3, 20, d3t1),        C(2, 4, 40, d3t2),       C(3, 5, 80, d3t3),
    C(4, 6, 160, d3t4),      C(5, 7, 672, d3t5),       C(6, 8, 1000, d3t6),     C(7, 9, 2688, d3t7),
    C(8, 10, 5376, d3t8),    C(9, 11, 10752, d3t9),    C(10, 12, 21504, d3t10), C(11, 13, 43008, d3t11),
    C(12, 14, 70000, d3t12), C(13, 15, 172032, d3t13), C(14, 16, 0xFFFFFFFFu, d3t14),
};
#undef C

typedef struct {
    const uint8_t *p, *end;
    uint64_t acc;
    int bits;
    bool err;
} d3bits;

static uint32_t d3_get(d3bits *b, int n)
{
    if (n == 0)
        return 0;
    while (b->bits < n) {
        if (b->p >= b->end)
            b->err = true; /* a stream that needs bytes past its end is damaged */
        b->acc = (b->acc << 8) | (b->p < b->end ? *b->p++ : 0);
        b->bits += 8;
    }
    b->bits -= n;
    return (uint32_t)((b->acc >> b->bits) & ((1ull << n) - 1));
}

/* A match length code, before the bias. */
static uint32_t d3_length(d3bits *b)
{
    int k = 0;
    while (k < 10 && d3_get(b, 1))
        k++;
    switch (k) {
    case 0:
        return d3_get(b, 1);
    case 1:
        return d3_get(b, 1) ? d3_get(b, 1) + 3 : 2;
    case 2:
        return d3_get(b, 1) ? d3_get(b, 2) + 7 : d3_get(b, 1) + 5;
    case 3:
        return d3_get(b, 3) + 11;
    case 4:
        return d3_get(b, 3) + 19;
    case 5:
        return d3_get(b, 5) + 27;
    case 6:
        return d3_get(b, 6) + 59;
    case 7:
        return d3_get(b, 7) + 123;
    case 8:
        return d3_get(b, 8) + 251;
    case 9:
        return d3_get(b, 9) + 507;
    default:
        return d3_get(b, 10) + 1019;
    }
}

/* The length of a literal run (1 or more). */
static uint32_t d3_literals(d3bits *b)
{
    if (!d3_get(b, 1))
        return 1;
    switch (d3_get(b, 2)) {
    case 0:
        return 2;
    case 1:
        return 3;
    case 2:
        return d3_get(b, 2) + 4;
    default: {
        uint32_t x = d3_get(b, 4);
        if (x <= 7)
            return x + 8;
        if (x <= 11)
            return (x - 8) * 4 + 16 + d3_get(b, 2);
        return (x - 12) * 8 + 32 + d3_get(b, 3);
    }
    }
}

/* The distance back from the end of the output (1 or more), `pos` bytes in. */
static uint32_t d3_distance(d3bits *b, uint32_t pos)
{
    const dcmp3_class *c = dcmp3_classes;
    while (pos > c->maxpos)
        c++;
    if (!d3_get(b, 1))
        return d3_get(b, c->n0) + 1;
    if (!d3_get(b, 1))
        return d3_get(b, c->n1) + (1u << c->n0) + 1;
    unsigned n = 0;
    while (n < c->nthr && pos > c->thr[n])
        n++;
    return d3_get(b, (int)n + 1) + (1u << c->n0) + (1u << c->n1) + 1;
}

static void dcmp3(dstate *s, uint32_t outlen)
{
    d3bits b = { s->p, s->end, 0, 0, false };
    bool after_long = true; /* the last token was not a short literal run */
    uint32_t pos = 0;
    while (pos < outlen && !b.err) {
        uint32_t len = d3_length(&b);
        if (len == 0 && after_long) {
            uint32_t n = d3_literals(&b);
            if (n > outlen - pos) {
                s->err = true;
                return;
            }
            for (uint32_t i = 0; i < n; i++)
                s->out[pos++] = (uint8_t)d3_get(&b, 8);
            after_long = n >= 63;
            continue;
        }
        len += after_long ? 2 : 3;
        after_long = true;
        uint32_t dist = d3_distance(&b, pos);
        if (dist > pos || len > outlen - pos) {
            s->err = true;
            return;
        }
        for (uint32_t i = 0; i < len; i++, pos++)
            s->out[pos] = s->out[pos - dist];
    }
    s->pos = pos;
    if (b.err)
        s->err = true;
}

/* The header length word is 0x12; some compressors left it 0 (their own
 * 'dcmp' ids, 128 and up) and the Resource Manager read them all the same. */
bool rsrc_is_compressed(const uint8_t *d, size_t len)
{
    return len >= 18 && be32(d) == 0xA89F6572u && (be16(d + 4) == 0x0012 || be16(d + 4) == 0) &&
           (be16(d + 6) == 0x0801 || be16(d + 6) == 0x0901);
}

uint32_t rsrc_decompressed_size(const uint8_t *d, size_t len)
{
    return rsrc_is_compressed(d, len) ? be32(d + 8) : (uint32_t)len;
}

bool rsrc_decompress(const uint8_t *d, size_t len, uint8_t **out, size_t *out_len, char *err, size_t errlen)
{
    if (!rsrc_is_compressed(d, len)) {
        snprintf(err, errlen, "not a compressed resource");
        return false;
    }
    uint16_t type = be16(d + 6);
    uint32_t outlen = be32(d + 8);
    int16_t id = type == 0x0801 ? (int16_t)be16(d + 14) : (int16_t)be16(d + 12);
    dstate s = { d + 18, d + len, malloc(outlen ? outlen : 1), 0, outlen, false, NULL, NULL, 0, 0 };
    if (!s.out) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    if (type == 0x0801 && id == 0)
        dcmp0(&s);
    else if (type == 0x0801 && id == 1)
        dcmp1(&s);
    else if (type == 0x0901 && id == 2)
        dcmp2(&s, d + 14, outlen);
    else if (type == 0x0901 && id == 3)
        dcmp3(&s, outlen);
    else {
        snprintf(err, errlen, "unsupported decompressor 'dcmp' %d", id);
        free(s.out);
        return false;
    }
    free(s.lit_off);
    free(s.lit_len);
    /* dcmp 0 works in 2-byte units; an odd size drops the final byte. */
    if (!s.err && !(s.pos == outlen || (s.pos == (size_t)outlen + 1 && (outlen & 1)))) {
        snprintf(err, errlen, "decompressed %zu bytes, expected %u", s.pos, outlen);
        free(s.out);
        return false;
    }
    if (s.err) {
        snprintf(err, errlen, "corrupt compressed resource ('dcmp' %d)", id);
        free(s.out);
        return false;
    }
    *out = s.out;
    *out_len = outlen;
    return true;
}
