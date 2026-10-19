/* An AppleShare (AFP 2.x) client: sessions over TCP (DSI) or AppleTalk
 * (ASP over ATP), and the calls a mirrored volume needs (afp.c, asp.c). */
#ifndef AFP_H
#define AFP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct afp_session afp_session;

/* AFP result codes are negative (kFPAccessDenied -5000 ...); 0 is noErr.
 * Transport failures are -5014 (kFPMiscErr) unless a better one fits:
 * -5016 kFPNoServer (no connection), -5022 kFPSessClosed (a session lost
 * and not opened again). */

/* Connect and log in: user NULL or "" for a guest ("No User Authent"),
 * else "Cleartxt Passwrd".  host "name" or "name:port" for TCP. */
afp_session *afp_connect_tcp(const char *host, const char *user, const char *password, char *err, size_t errlen);
/* Over AppleTalk: the server's NBP name ("Appleshare:AFPServer@*"), looked
 * up and reached with the machine's AppleTalk (which must be on). */
afp_session *afp_connect_atalk(const char *nbp_name, const char *user, const char *password, char *err,
                               size_t errlen);
void afp_close(afp_session *s);
/* Keep an idle session open: call every few seconds (it sends a tickle
 * when nothing has gone to the server for 30 seconds); servers close
 * sessions not heard from in about two minutes.  A lost session is opened
 * again by the next call anyway (its volumes too, by name), but not its
 * open forks, which no call here leaves open. */
void afp_idle(afp_session *s);

/* The server's volumes (MacRoman Pascal-less names, NUL-terminated). */
int afp_volumes(afp_session *s, char (**names)[28], int *count);
int afp_open_volume(afp_session *s, const char *name, uint16_t *volid);
int afp_close_volume(afp_session *s, uint16_t vol);

typedef struct {
    bool dir;
    uint32_t id;            /* directory ID, or file number */
    uint32_t parent;
    char name[32];          /* MacRoman, NUL-terminated */
    uint8_t finder_info[32];
    uint32_t data_len, rsrc_len; /* files */
    uint32_t create_date, modify_date; /* Mac times (seconds since 1904, local) */
} afp_entry;

/* A directory's contents (2 is a volume's root); *entries malloc'd. */
int afp_list(afp_session *s, uint16_t vol, uint32_t dir, afp_entry **entries, int *count);
/* One file or directory by name in a directory. */
int afp_stat(afp_session *s, uint16_t vol, uint32_t dir, const char *name, afp_entry *out);
/* A whole fork read (*data malloc'd), or written (replacing it). */
int afp_read_fork(afp_session *s, uint16_t vol, uint32_t dir, const char *name, bool rsrc, uint8_t **data,
                  size_t *len);
int afp_write_fork(afp_session *s, uint16_t vol, uint32_t dir, const char *name, bool rsrc, const uint8_t *data,
                   size_t len);
int afp_set_finder_info(afp_session *s, uint16_t vol, uint32_t dir, const char *name, const uint8_t info[32]);
int afp_create_file(afp_session *s, uint16_t vol, uint32_t dir, const char *name);
int afp_create_dir(afp_session *s, uint16_t vol, uint32_t dir, const char *name, uint32_t *newid);
int afp_delete(afp_session *s, uint16_t vol, uint32_t dir, const char *name);
/* Rename and/or move (to another directory on the volume). */
int afp_move(afp_session *s, uint16_t vol, uint32_t dir, const char *name, uint32_t to_dir, const char *new_name);

#endif
