/*
 * The catalog (catalog.c, SQLite): folders' Finder information and
 * comments, and the volumes' desktop databases, by host path.
 */
#ifndef CATALOG_H
#define CATALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void catalog_set_path(const char *path);
void catalog_close(void);

bool catalog_get_dirinfo(const char *path, uint8_t finfo[16], uint8_t fxinfo[16]);
bool catalog_set_dirinfo(const char *path, const uint8_t *finfo, const uint8_t *fxinfo);
int catalog_get_comment(const char *path, uint8_t *text, int max);
bool catalog_set_comment(const char *path, const uint8_t *text, int len);
void catalog_moved(const char *from, const char *to);

bool catalog_dt_open(const char *root);
void catalog_dt_reset(const char *root, bool remove_all);
bool catalog_dt_add_appl(const char *root, uint32_t creator, const char *path, uint32_t tag);
bool catalog_dt_remove_appl(const char *root, uint32_t creator, const char *path);
bool catalog_dt_appl(const char *root, uint32_t creator, int index, char *path, size_t size, uint32_t *tag);
bool catalog_dt_add_icon(const char *root, uint32_t creator, uint32_t type, int kind, uint32_t tag, const uint8_t *data,
                         int len);
void catalog_dt_icons(const char *root, void (*each)(uint32_t creator, uint32_t type, int kind, uint32_t tag,
                                                     const uint8_t *data, int len, void *ctx),
                      void *ctx);
int catalog_dt_counts(const char *root, int *appls, int *icons);

#endif
