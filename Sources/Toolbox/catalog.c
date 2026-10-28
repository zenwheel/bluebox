/*
 * The catalog (SQLite): what the File Manager keeps about host files that
 * the files themselves have no place for.  A folder has no AppleDouble
 * sidecar, so its Finder information (the window the Finder opens for it,
 * its view and scroll, its icon's place, label and flags) and its comment
 * live here, by host path.  So does each volume's desktop database: the
 * applications and icons programs (the Finder's rebuild above all) add,
 * and whether the database has been built.  The file is Catalog.sqlite
 * beside the Macintosh HD folder; when it cannot be opened the File
 * Manager carries on without it, as before.
 */
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog.h"

static sqlite3 *db;
static bool tried;
static char db_path[4096];

void catalog_set_path(const char *path)
{
    catalog_close();
    snprintf(db_path, sizeof db_path, "%s", path ? path : "");
}

void catalog_close(void)
{
    if (db)
        sqlite3_close(db);
    db = NULL;
    tried = false;
}

static sqlite3 *open_db(void)
{
    if (db || tried)
        return db;
    tried = true;
    if (!db_path[0] || sqlite3_open(db_path, &db) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        db = NULL;
        return NULL;
    }
    sqlite3_busy_timeout(db, 2000); /* other runs share it */
    static const char schema[] =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "CREATE TABLE IF NOT EXISTS nodes(rel TEXT PRIMARY KEY, id INTEGER UNIQUE);"
        "CREATE TABLE IF NOT EXISTS dirinfo(path TEXT PRIMARY KEY, finfo BLOB, fxinfo BLOB);"
        "CREATE TABLE IF NOT EXISTS comments(path TEXT PRIMARY KEY, text BLOB);"
        "CREATE TABLE IF NOT EXISTS dt_volumes(root TEXT PRIMARY KEY);"
        "CREATE TABLE IF NOT EXISTS dt_appls(root TEXT, creator INTEGER, path TEXT, tag INTEGER,"
        " PRIMARY KEY(root, creator, path));"
        "CREATE TABLE IF NOT EXISTS dt_icons(root TEXT, creator INTEGER, type INTEGER, kind INTEGER, tag INTEGER,"
        " data BLOB, PRIMARY KEY(root, creator, type, kind));";
    if (sqlite3_exec(db, schema, NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        db = NULL;
    }
    return db;
}

static sqlite3_stmt *prep(const char *sql)
{
    sqlite3_stmt *st = NULL;
    if (!open_db() || sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return NULL;
    return st;
}

/* One statement run to its end; true when it succeeded. */
static bool run(sqlite3_stmt *st)
{
    if (!st)
        return false;
    int r;
    while ((r = sqlite3_step(st)) == SQLITE_ROW)
        ;
    sqlite3_finalize(st);
    return r == SQLITE_DONE;
}

/* ---- Stable IDs ----
 * The directory and file IDs (CNIDs) the File Manager gives the volume's
 * folders and files, by their path on it, so they are the same from run
 * to run: aliases, Put Away and file ID references keep them. */

void catalog_nodes(void (*each)(const char *rel, int32_t id, void *ctx), void *ctx)
{
    sqlite3_stmt *st = prep("SELECT rel, id FROM nodes");
    if (!st)
        return;
    while (sqlite3_step(st) == SQLITE_ROW)
        each((const char *)sqlite3_column_text(st, 0), sqlite3_column_int(st, 1), ctx);
    sqlite3_finalize(st);
}

/* Taken for this path, or false when the path or the ID is another's. */
bool catalog_add_node(const char *rel, int32_t id)
{
    sqlite3_stmt *st = prep("INSERT INTO nodes(rel, id) VALUES(?, ?)");
    if (!st)
        return false;
    sqlite3_bind_text(st, 1, rel, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, id);
    return run(st);
}

/* A path's ID, or 0; and the highest ID given. */
int32_t catalog_node_id(const char *rel)
{
    sqlite3_stmt *st = rel ? prep("SELECT id FROM nodes WHERE rel = ?") : prep("SELECT max(id) FROM nodes");
    if (!st)
        return 0;
    if (rel)
        sqlite3_bind_text(st, 1, rel, -1, SQLITE_TRANSIENT);
    int32_t id = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : 0;
    sqlite3_finalize(st);
    return id;
}

/* The IDs of what is at `from` and inside it, now at `to` (NULL: gone). */
void catalog_nodes_moved(const char *from, const char *to)
{
    sqlite3_stmt *st = to ? prep("UPDATE OR REPLACE nodes SET rel = ?2 || substr(rel, length(?1) + 1)"
                                 " WHERE rel = ?1 OR substr(rel, 1, length(?1) + 1) = ?1 || '/'")
                          : prep("DELETE FROM nodes WHERE rel = ?1 OR substr(rel, 1, length(?1) + 1) = ?1 || '/'");
    if (!st)
        return;
    sqlite3_bind_text(st, 1, from, -1, SQLITE_TRANSIENT);
    if (to)
        sqlite3_bind_text(st, 2, to, -1, SQLITE_TRANSIENT);
    run(st);
}

/* ---- Folders' Finder information ---- */

bool catalog_get_dirinfo(const char *path, uint8_t finfo[16], uint8_t fxinfo[16])
{
    sqlite3_stmt *st = prep("SELECT finfo, fxinfo FROM dirinfo WHERE path = ?");
    if (!st)
        return false;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        if (sqlite3_column_bytes(st, 0) == 16)
            memcpy(finfo, sqlite3_column_blob(st, 0), 16);
        if (sqlite3_column_bytes(st, 1) == 16)
            memcpy(fxinfo, sqlite3_column_blob(st, 1), 16);
        found = true;
    }
    sqlite3_finalize(st);
    return found;
}

bool catalog_set_dirinfo(const char *path, const uint8_t *finfo, const uint8_t *fxinfo)
{
    uint8_t fi[16] = { 0 }, fx[16] = { 0 };
    catalog_get_dirinfo(path, fi, fx);
    if (finfo)
        memcpy(fi, finfo, 16);
    if (fxinfo)
        memcpy(fx, fxinfo, 16);
    sqlite3_stmt *st = prep("INSERT OR REPLACE INTO dirinfo(path, finfo, fxinfo) VALUES(?, ?, ?)");
    if (!st)
        return false;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(st, 2, fi, 16, SQLITE_TRANSIENT);
    sqlite3_bind_blob(st, 3, fx, 16, SQLITE_TRANSIENT);
    return run(st);
}

/* ---- Comments (folders') ---- */

int catalog_get_comment(const char *path, uint8_t *text, int max)
{
    sqlite3_stmt *st = prep("SELECT text FROM comments WHERE path = ?");
    if (!st)
        return 0;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    int n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_bytes(st, 0);
        if (n > max)
            n = max;
        memcpy(text, sqlite3_column_blob(st, 0), (size_t)n);
    }
    sqlite3_finalize(st);
    return n;
}

bool catalog_set_comment(const char *path, const uint8_t *text, int len)
{
    sqlite3_stmt *st = len ? prep("INSERT OR REPLACE INTO comments(path, text) VALUES(?, ?)")
                           : prep("DELETE FROM comments WHERE path = ?");
    if (!st)
        return false;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    if (len)
        sqlite3_bind_blob(st, 2, text, len, SQLITE_TRANSIENT);
    return run(st);
}

/* ---- Renames, moves and deletions ---- */

/* What is kept by the path of `from` and of everything inside it, now at
 * `to` (to NULL: forgotten). */
void catalog_moved(const char *from, const char *to)
{
    if (!open_db())
        return;
    static const char *const tables[] = { "dirinfo", "comments" };
    for (size_t i = 0; i < sizeof tables / sizeof tables[0]; i++) {
        char sql[256];
        if (to)
            snprintf(sql, sizeof sql,
                     "UPDATE OR REPLACE %s SET path = ?2 || substr(path, length(?1) + 1)"
                     " WHERE path = ?1 OR substr(path, 1, length(?1) + 1) = ?1 || '/'",
                     tables[i]);
        else
            snprintf(sql, sizeof sql, "DELETE FROM %s WHERE path = ?1 OR substr(path, 1, length(?1) + 1) = ?1 || '/'",
                     tables[i]);
        sqlite3_stmt *st = prep(sql);
        if (!st)
            continue;
        sqlite3_bind_text(st, 1, from, -1, SQLITE_TRANSIENT);
        if (to)
            sqlite3_bind_text(st, 2, to, -1, SQLITE_TRANSIENT);
        run(st);
    }
}

/* ---- Desktop databases ---- */

/* Whether the volume's database had been built; it is from now on. */
bool catalog_dt_open(const char *root)
{
    sqlite3_stmt *st = prep("SELECT 1 FROM dt_volumes WHERE root = ?");
    if (!st)
        return true;
    sqlite3_bind_text(st, 1, root, -1, SQLITE_TRANSIENT);
    bool built = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    if (!built && (st = prep("INSERT OR IGNORE INTO dt_volumes(root) VALUES(?)"))) {
        sqlite3_bind_text(st, 1, root, -1, SQLITE_TRANSIENT);
        run(st);
    }
    return built;
}

/* PBDTReset: applications and icons gone, comments kept; PBDTDelete:
 * everything, and the database is new again. */
void catalog_dt_reset(const char *root, bool remove_all)
{
    static const char *const sql[] = { "DELETE FROM dt_appls WHERE root = ?", "DELETE FROM dt_icons WHERE root = ?",
                                       "DELETE FROM dt_volumes WHERE root = ?" };
    for (int i = 0; i < (remove_all ? 3 : 2); i++) {
        sqlite3_stmt *st = prep(sql[i]);
        if (!st)
            continue;
        sqlite3_bind_text(st, 1, root, -1, SQLITE_TRANSIENT);
        run(st);
    }
    if (remove_all) {
        sqlite3_stmt *st = prep("DELETE FROM comments WHERE substr(path, 1, length(?1) + 1) = ?1 || '/'");
        if (st) {
            sqlite3_bind_text(st, 1, root, -1, SQLITE_TRANSIENT);
            run(st);
        }
    }
}

bool catalog_dt_add_appl(const char *root, uint32_t creator, const char *path, uint32_t tag)
{
    sqlite3_stmt *st = prep("INSERT OR REPLACE INTO dt_appls(root, creator, path, tag) VALUES(?, ?, ?, ?)");
    if (!st)
        return false;
    sqlite3_bind_text(st, 1, root, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, creator);
    sqlite3_bind_text(st, 3, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, tag);
    return run(st);
}

bool catalog_dt_remove_appl(const char *root, uint32_t creator, const char *path)
{
    sqlite3_stmt *st = prep("DELETE FROM dt_appls WHERE root = ? AND creator = ? AND path = ?");
    if (!st)
        return false;
    sqlite3_bind_text(st, 1, root, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, creator);
    sqlite3_bind_text(st, 3, path, -1, SQLITE_TRANSIENT);
    bool ok = run(st);
    return ok && sqlite3_changes(db) > 0;
}

/* The index-th (from 0) application added for a creator, most recent first. */
bool catalog_dt_appl(const char *root, uint32_t creator, int index, char *path, size_t size, uint32_t *tag)
{
    sqlite3_stmt *st =
        prep("SELECT path, tag FROM dt_appls WHERE root = ? AND creator = ? ORDER BY rowid DESC LIMIT 1 OFFSET ?");
    if (!st)
        return false;
    sqlite3_bind_text(st, 1, root, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, creator);
    sqlite3_bind_int(st, 3, index);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        snprintf(path, size, "%s", (const char *)sqlite3_column_text(st, 0));
        if (tag)
            *tag = (uint32_t)sqlite3_column_int64(st, 1);
        found = true;
    }
    sqlite3_finalize(st);
    return found;
}

bool catalog_dt_add_icon(const char *root, uint32_t creator, uint32_t type, int kind, uint32_t tag, const uint8_t *data,
                         int len)
{
    sqlite3_stmt *st =
        prep("INSERT OR REPLACE INTO dt_icons(root, creator, type, kind, tag, data) VALUES(?, ?, ?, ?, ?, ?)");
    if (!st)
        return false;
    sqlite3_bind_text(st, 1, root, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, creator);
    sqlite3_bind_int64(st, 3, type);
    sqlite3_bind_int(st, 4, kind);
    sqlite3_bind_int64(st, 5, tag);
    sqlite3_bind_blob(st, 6, data, len, SQLITE_TRANSIENT);
    return run(st);
}

/* Each icon kept for a volume, in turn (the File Manager adds them to the
 * run's own list). */
void catalog_dt_icons(const char *root, void (*each)(uint32_t creator, uint32_t type, int kind, uint32_t tag,
                                                     const uint8_t *data, int len, void *ctx),
                      void *ctx)
{
    sqlite3_stmt *st = prep("SELECT creator, type, kind, tag, data FROM dt_icons WHERE root = ?");
    if (!st)
        return;
    sqlite3_bind_text(st, 1, root, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW)
        each((uint32_t)sqlite3_column_int64(st, 0), (uint32_t)sqlite3_column_int64(st, 1), sqlite3_column_int(st, 2),
             (uint32_t)sqlite3_column_int64(st, 3), sqlite3_column_blob(st, 4), sqlite3_column_bytes(st, 4), ctx);
    sqlite3_finalize(st);
}

/* The rows a volume's database holds, for PBDTGetInfo. */
int catalog_dt_counts(const char *root, int *appls, int *icons)
{
    *appls = *icons = 0;
    sqlite3_stmt *st = prep("SELECT (SELECT count(*) FROM dt_appls WHERE root = ?1),"
                            " (SELECT count(*) FROM dt_icons WHERE root = ?1),"
                            " (SELECT coalesce(sum(length(data)), 0) FROM dt_icons WHERE root = ?1)");
    if (!st)
        return 0;
    sqlite3_bind_text(st, 1, root, -1, SQLITE_TRANSIENT);
    int bytes = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        *appls = sqlite3_column_int(st, 0);
        *icons = sqlite3_column_int(st, 1);
        bytes = sqlite3_column_int(st, 2);
    }
    sqlite3_finalize(st);
    return bytes;
}
