/* ASP, the AppleTalk Session Protocol (asp.c): the AppleTalk transport
 * under afp.c's AFP calls. */
#ifndef ASP_H
#define ASP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The largest command and reply: one and eight ATP packets. */
enum { ASP_CMD_MAX = 578, ASP_QUANTUM = 8 * 578 };
/* AFP's codes for a session's failures, as afp.c reports them. */
enum { ASP_ERR_MISC = -5014, ASP_ERR_NO_SERVER = -5016, ASP_ERR_SESS_CLOSED = -5022 };

typedef struct asp_session asp_session;

/* Look the server up by NBP name ("object:type@zone"), ask its status
 * (status and *status_len optional, up to ASP_QUANTUM bytes), and open a
 * session. */
asp_session *asp_open(const char *nbp_name, uint8_t *status, int *status_len, char *err, size_t errlen);
/* The same at a server's address (its node and session listening socket),
 * asking every interval_ms, tries times: NULL with the server's refusal or
 * the transport's error in *err. */
asp_session *asp_open_at(uint8_t node, uint8_t sls, int interval_ms, int tries, int16_t *err);
/* A server's status, up to cap bytes. */
int16_t asp_status_at(uint8_t node, uint8_t sls, uint8_t *status, int cap, int *status_len);
/* The server's attentions, to fn (called while the stack runs). */
void asp_set_attention(asp_session *s, void (*fn)(void *ctx, uint16_t code), void *ctx);
/* A command: the AFP result from the reply's CmdResult (or a session
 * failure), and the reply data (up to cap bytes; *rlen its length). */
int asp_command(asp_session *s, const uint8_t *cmd, int len, uint8_t *reply, int cap, int *rlen);
/* A command with data the server asks for (WriteContinue): FPWrite. */
int asp_write(asp_session *s, const uint8_t *cmd, int len, const uint8_t *data, int dlen, uint8_t *reply, int cap,
              int *rlen);
/* A tickle for the server, which closes a session it has not heard from. */
void asp_tickle(asp_session *s);
bool asp_alive(asp_session *s);
void asp_close(asp_session *s);

#endif
