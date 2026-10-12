#include "forks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

/* CRC-16/XMODEM, as used by MacBinary II. */
static uint16_t crc_xmodem(const uint8_t *p, size_t n)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)(p[i] << 8);
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

bool macbinary_detect(const uint8_t *b, size_t len)
{
    if (len < 128 || b[0] != 0 || b[74] != 0 || b[1] < 1 || b[1] > 63)
        return false;
    uint32_t dl = be32(b + 83), rl = be32(b + 87);
    if (dl > 0x7FFFFFFF || rl > 0x00FFFFFF)
        return false;
    if (be16(b + 124) == crc_xmodem(b, 124))
        return true; /* MacBinary II/III */
    return b[82] == 0; /* MacBinary I */
}

static size_t round128(size_t n) { return (n + 127) & ~(size_t)127; }

bool macbinary_parse(const uint8_t *b, size_t len, mac_file *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof *out);
    if (!macbinary_detect(b, len)) {
        snprintf(err, errlen, "not a MacBinary file");
        return false;
    }
    uint32_t dl = be32(b + 83), rl = be32(b + 87);
    size_t sec = 0;
    if (be16(b + 124) == crc_xmodem(b, 124))
        sec = round128(be16(b + 120));
    size_t data_at = 128 + sec;
    size_t rsrc_at = data_at + round128(dl);
    if (rsrc_at + rl > len && !(rl == 0 && data_at + dl <= len)) {
        snprintf(err, errlen, "MacBinary file is truncated");
        return false;
    }
    memcpy(out->name, b + 2, b[1]);
    out->name[b[1]] = 0;
    /* FInfo: type, creator, flags (hi from 73, lo from 101), location, folder. */
    memcpy(out->finder_info, b + 65, 8);
    out->finder_info[8] = b[73];
    out->finder_info[9] = b[101];
    memcpy(out->finder_info + 10, b + 75, 6);
    out->finder_info[16 + 8] = b[106]; /* fdScript (MacBinary III) */
    out->finder_info[16 + 9] = b[107]; /* fdXFlags (MacBinary III) */
    out->has_finder_info = true;
    out->create_date = be32(b + 91);
    out->modify_date = be32(b + 95);
    out->data = malloc(dl ? dl : 1);
    out->rsrc = malloc(rl ? rl : 1);
    if (!out->data || !out->rsrc) {
        mac_file_free(out);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    memcpy(out->data, b + data_at, dl);
    memcpy(out->rsrc, b + rsrc_at, rl);
    out->data_len = dl;
    out->rsrc_len = rl;
    return true;
}

void mac_file_free(mac_file *f)
{
    free(f->data);
    free(f->rsrc);
    f->data = f->rsrc = NULL;
    f->data_len = f->rsrc_len = 0;
}

bool read_file(const char *path, uint8_t **buf, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return false;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    long n = ftell(fp);
    rewind(fp);
    if (n < 0) {
        fclose(fp);
        return false;
    }
    uint8_t *p = malloc((size_t)n + 1);
    if (!p) {
        fclose(fp);
        return false;
    }
    if (fread(p, 1, (size_t)n, fp) != (size_t)n) {
        free(p);
        fclose(fp);
        return false;
    }
    fclose(fp);
    *buf = p;
    *len = (size_t)n;
    return true;
}
