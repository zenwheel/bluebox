#include "forks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

bool rsrc_parse(const uint8_t *f, size_t len, rsrc_index *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof *out);
    if (len == 0)
        return true; /* empty fork: no resources */
    if (len < 16) {
        snprintf(err, errlen, "resource fork too short");
        return false;
    }
    out->data_start = be32(f);
    out->map_start = be32(f + 4);
    out->data_len = be32(f + 8);
    out->map_len = be32(f + 12);
    if ((uint64_t)out->data_start + out->data_len > len || (uint64_t)out->map_start + out->map_len > len ||
        out->map_len < 30) {
        snprintf(err, errlen, "resource fork header out of range");
        return false;
    }
    const uint8_t *m = f + out->map_start;
    size_t mlen = out->map_len;
    out->file_attrs = be16(m + 22);
    uint16_t tlo = be16(m + 24), nlo = be16(m + 26);
    if ((size_t)tlo + 2 > mlen) {
        snprintf(err, errlen, "resource type list out of range");
        return false;
    }
    unsigned ntypes = (uint16_t)(be16(m + tlo) + 1);
    if (be16(m + tlo) == 0xFFFF)
        ntypes = 0;
    size_t total = 0;
    for (unsigned i = 0; i < ntypes; i++) {
        size_t te = (size_t)tlo + 2 + i * 8;
        if (te + 8 > mlen) {
            snprintf(err, errlen, "resource type entry out of range");
            return false;
        }
        total += (size_t)be16(m + te + 4) + 1;
    }
    out->entries = calloc(total ? total : 1, sizeof(rsrc_entry));
    if (!out->entries) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    for (unsigned i = 0; i < ntypes; i++) {
        size_t te = (size_t)tlo + 2 + i * 8;
        uint32_t type = be32(m + te);
        unsigned count = (unsigned)be16(m + te + 4) + 1;
        size_t rl = (size_t)tlo + be16(m + te + 6);
        for (unsigned j = 0; j < count; j++) {
            size_t re = rl + j * 12;
            if (re + 12 > mlen) {
                snprintf(err, errlen, "resource reference out of range");
                rsrc_index_free(out);
                return false;
            }
            rsrc_entry *e = &out->entries[out->count++];
            e->type = type;
            e->id = (int16_t)be16(m + re);
            uint16_t noff = be16(m + re + 2);
            e->attrs = m[re + 4];
            e->data_off = be32(m + re + 4) & 0x00FFFFFF;
            if ((uint64_t)e->data_off + 4 > out->data_len) {
                snprintf(err, errlen, "resource data offset out of range");
                rsrc_index_free(out);
                return false;
            }
            e->data_len = be32(f + out->data_start + e->data_off);
            if ((uint64_t)e->data_off + 4 + e->data_len > out->data_len) {
                snprintf(err, errlen, "resource data length out of range");
                rsrc_index_free(out);
                return false;
            }
            if (noff != 0xFFFF) {
                size_t np = (size_t)nlo + noff;
                if (np < mlen) {
                    size_t nl = m[np];
                    if (np + 1 + nl > mlen)
                        nl = mlen - np - 1;
                    memcpy(e->name, m + np + 1, nl);
                    e->name[nl] = 0;
                }
            }
        }
    }
    return true;
}

void rsrc_index_free(rsrc_index *idx)
{
    free(idx->entries);
    idx->entries = NULL;
    idx->count = 0;
}

const uint8_t *rsrc_data(const uint8_t *fork, const rsrc_index *idx, const rsrc_entry *e)
{
    return fork + idx->data_start + e->data_off + 4;
}

/* ---------------------------------------------------------------------- */
/* Building forks */

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

bool rsrc_build(const rsrc_spec *specs, size_t n, uint8_t **out, size_t *out_len)
{
    /* Distinct types in first-seen order. */
    uint32_t *types = malloc((n ? n : 1) * sizeof *types);
    size_t ntypes = 0, data_len = 0, names_len = 0;
    for (size_t i = 0; i < n; i++) {
        bool seen = false;
        for (size_t t = 0; t < ntypes; t++)
            if (types[t] == specs[i].type)
                seen = true;
        if (!seen)
            types[ntypes++] = specs[i].type;
        data_len += 4 + specs[i].len;
        if (specs[i].name && specs[i].name[0])
            names_len += 1 + strlen(specs[i].name);
    }
    size_t tl = 2 + ntypes * 8;
    size_t names_at = 28 + tl + n * 12;
    size_t map_len = names_at + names_len;
    size_t total = 256 + data_len + map_len;
    uint8_t *b = calloc(1, total);
    if (!b) {
        free(types);
        return false;
    }
    uint8_t *data = b + 256, *map = b + 256 + data_len;
    put32(b, 256);
    put32(b + 4, (uint32_t)(256 + data_len));
    put32(b + 8, (uint32_t)data_len);
    put32(b + 12, (uint32_t)map_len);
    memcpy(map, b, 16);
    put16(map + 24, 28);
    put16(map + 26, (uint16_t)names_at);
    put16(map + 28, (uint16_t)(ntypes ? ntypes - 1 : 0xFFFF));
    size_t doff = 0, noff = 0, ref = 0;
    for (size_t t = 0; t < ntypes; t++) {
        size_t count = 0, first_ref = ref;
        for (size_t i = 0; i < n; i++) {
            const rsrc_spec *e = &specs[i];
            if (e->type != types[t])
                continue;
            uint8_t *r = map + 28 + tl + ref * 12;
            put16(r, (uint16_t)e->id);
            if (e->name && e->name[0]) {
                size_t nl = strlen(e->name);
                if (nl > 255)
                    nl = 255;
                put16(r + 2, (uint16_t)noff);
                map[names_at + noff] = (uint8_t)nl;
                memcpy(map + names_at + noff + 1, e->name, nl);
                noff += 1 + nl;
            } else {
                put16(r + 2, 0xFFFF);
            }
            put32(r + 4, (uint32_t)doff);
            r[4] = e->attrs;
            put32(data + doff, e->len);
            if (e->len)
                memcpy(data + doff + 4, e->data, e->len);
            doff += 4 + e->len;
            ref++;
            count++;
        }
        uint8_t *te = map + 28 + 2 + t * 8;
        put32(te, types[t]);
        put16(te + 4, (uint16_t)(count - 1));
        put16(te + 6, (uint16_t)(tl + first_ref * 12));
    }
    free(types);
    *out = b;
    *out_len = total;
    return true;
}

bool rsrc_filter(const uint8_t *fork, size_t len, const uint32_t *types, size_t ntypes, uint8_t **out,
                 size_t *out_len, char *err, size_t errlen)
{
    rsrc_index idx;
    if (!rsrc_parse(fork, len, &idx, err, errlen))
        return false;
    rsrc_spec *specs = malloc((idx.count ? idx.count : 1) * sizeof *specs);
    size_t n = 0;
    for (size_t t = 0; t < ntypes; t++)
        for (size_t i = 0; i < idx.count; i++)
            if (idx.entries[i].type == types[t])
                specs[n++] = (rsrc_spec){ types[t], idx.entries[i].id, idx.entries[i].name, idx.entries[i].attrs,
                                          rsrc_data(fork, &idx, &idx.entries[i]), idx.entries[i].data_len };
    bool ok = rsrc_build(specs, n, out, out_len);
    if (!ok)
        snprintf(err, errlen, "out of memory");
    free(specs);
    rsrc_index_free(&idx);
    return ok;
}
