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
