#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diskimage.h"

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

/* ---------------------------------------------------------------------- */
/* Apple Data Compression */

long adc_decompress(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen)
{
    size_t i = 0, o = 0;
    while (i < inlen) {
        uint8_t b = in[i];
        size_t len, dist;
        if (b & 0x80) { /* literal run */
            len = (size_t)(b & 0x7F) + 1;
            if (i + 1 + len > inlen || o + len > outlen)
                return -1;
            memcpy(out + o, in + i + 1, len);
            i += 1 + len;
            o += len;
            continue;
        }
        if (b & 0x40) { /* three-byte code: long distance */
            if (i + 3 > inlen)
                return -1;
            len = (size_t)(b & 0x3F) + 4;
            dist = ((size_t)in[i + 1] << 8 | in[i + 2]) + 1;
            i += 3;
        } else { /* two-byte code */
            if (i + 2 > inlen)
                return -1;
            len = (size_t)((b >> 2) & 0x0F) + 3;
            dist = ((size_t)(b & 3) << 8 | in[i + 1]) + 1;
            i += 2;
        }
        if (dist > o || o + len > outlen)
            return -1;
        for (size_t k = 0; k < len; k++, o++) /* may overlap */
            out[o] = out[o - dist];
    }
    return (long)o;
}

/* ---------------------------------------------------------------------- */
/* Detection */

static const uint8_t *find_resource(const mac_file *f, uint32_t type, size_t *len)
{
    rsrc_index idx;
    char err[64];
    if (!f->rsrc_len || !rsrc_parse(f->rsrc, f->rsrc_len, &idx, err, sizeof err))
        return NULL;
    const uint8_t *found = NULL;
    for (size_t i = 0; i < idx.count; i++) {
        if (idx.entries[i].type == type) {
            found = rsrc_data(f->rsrc, &idx, &idx.entries[i]);
            *len = idx.entries[i].data_len;
            break;
        }
    }
    rsrc_index_free(&idx);
    return found;
}

static bool hfs_sig_at(const uint8_t *d, size_t len, size_t off)
{
    /* 'BD' = HFS, 'H+' = HFS+, 'HX' = HFSX; 0xD2D7 = MFS (recognised, not yet readable) */
    if (len < off + 1024 + 2)
        return false;
    uint16_t sig = be16(d + off + 1024);
    return sig == 0x4244 || sig == 0x482B || sig == 0x4858;
}

static bool apm_at(const uint8_t *d, size_t len)
{
    return len >= 1024 && be16(d) == 0x4552 /* 'ER' */ && be16(d + 512) == 0x504D /* 'PM' */;
}

disk_kind diskimage_detect(const mac_file *f)
{
    size_t blen;
    if (find_resource(f, 0x6263656D /* 'bcem' */, &blen))
        return DISK_NDIF;
    const uint8_t *d = f->data;
    size_t n = f->data_len;
    if (n >= 84 && d[0] <= 63 && be16(d + 82) == 0x0100 && be32(d + 64) && (size_t)be32(d + 64) + 84 <= n)
        return DISK_DC42;
    if (hfs_sig_at(d, n, 0) || apm_at(d, n))
        return DISK_RAW;
    return DISK_NONE;
}

/* ---------------------------------------------------------------------- */
/* Decoding */

static bool decode_ndif(const mac_file *f, uint8_t **vol, size_t *len, char *err, size_t errlen)
{
    size_t blen;
    const uint8_t *b = find_resource(f, 0x6263656D, &blen);
    if (!b || blen < 0x80) {
        snprintf(err, errlen, "NDIF image without a usable 'bcem' resource");
        return false;
    }
    uint32_t sectors = be32(b + 0x44);
    uint32_t nchunks = be32(b + 0x7C);
    if ((size_t)0x80 + (size_t)nchunks * 12 > blen || sectors == 0 || sectors > (1u << 23)) {
        snprintf(err, errlen, "NDIF chunk table out of range");
        return false;
    }
    size_t total = (size_t)sectors * 512;
    uint8_t *out = calloc(1, total);
    if (!out) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    for (uint32_t i = 0; i < nchunks; i++) {
        const uint8_t *e = b + 0x80 + i * 12;
        uint32_t start = be32(e) >> 8;
        uint8_t type = e[3];
        uint32_t off = be32(e + 4), clen = be32(e + 8);
        if (type == 0xFF)
            break;
        uint32_t next = i + 1 < nchunks ? be32(e + 12) >> 8 : sectors;
        if (next < start || next > sectors || (uint64_t)off + clen > f->data_len) {
            snprintf(err, errlen, "NDIF chunk %u out of range", i);
            free(out);
            return false;
        }
        size_t want = (size_t)(next - start) * 512;
        uint8_t *dst = out + (size_t)start * 512;
        switch (type) {
        case 0x00: /* zero-filled */
        case 0x01:
            break;
        case 0x02: /* raw */
            memcpy(dst, f->data + off, clen < want ? clen : want);
            break;
        case 0x83: /* ADC */
            if (adc_decompress(f->data + off, clen, dst, want) < 0) {
                snprintf(err, errlen, "NDIF chunk %u: corrupt ADC data", i);
                free(out);
                return false;
            }
            break;
        default:
            snprintf(err, errlen, "NDIF chunk type 0x%02X is not supported", type);
            free(out);
            return false;
        }
    }
    *vol = out;
    *len = total;
    return true;
}

/* Find the first Apple_HFS partition in an Apple partition map. */
static bool apm_hfs(const uint8_t *d, size_t n, size_t *off, size_t *plen)
{
    uint32_t count = be32(d + 512 + 4);
    for (uint32_t i = 1; i <= count && (size_t)(i + 1) * 512 <= n; i++) {
        const uint8_t *p = d + (size_t)i * 512;
        if (be16(p) != 0x504D)
            break;
        if (!strncmp((const char *)p + 48, "Apple_HFS", 32)) {
            *off = (size_t)be32(p + 8) * 512;
            *plen = (size_t)be32(p + 12) * 512;
            return *off < n;
        }
    }
    return false;
}

bool diskimage_locate_raw(const uint8_t *d, size_t n, size_t *off, size_t *len)
{
    *off = 0;
    *len = n;
    if (hfs_sig_at(d, n, 0))
        return true;
    if (apm_at(d, n) && apm_hfs(d, n, off, len)) {
        if (*off + *len > n)
            *len = n - *off;
        return true;
    }
    return false;
}

bool diskimage_decode(const mac_file *f, uint8_t **vol, size_t *len, char *err, size_t errlen)
{
    const uint8_t *d = f->data;
    size_t n = f->data_len, off = 0, plen = n;
    switch (diskimage_detect(f)) {
    case DISK_NDIF:
        if (!decode_ndif(f, vol, len, err, errlen))
            return false;
        d = *vol;
        n = *len;
        if (apm_at(d, n) && apm_hfs(d, n, &off, &plen)) {
            if (off + plen > n)
                plen = n - off;
            memmove(*vol, *vol + off, plen);
            *len = plen;
        }
        return true;
    case DISK_DC42:
        off = 84;
        plen = be32(d + 64);
        break;
    case DISK_RAW:
        if (apm_at(d, n) && !hfs_sig_at(d, n, 0) && !apm_hfs(d, n, &off, &plen)) {
            snprintf(err, errlen, "partition map without an HFS partition");
            return false;
        }
        break;
    default:
        snprintf(err, errlen, "not a recognised disk image");
        return false;
    }
    if (off + plen > n)
        plen = n - off;
    *vol = malloc(plen ? plen : 1);
    if (!*vol) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    memcpy(*vol, d + off, plen);
    *len = plen;
    return true;
}
