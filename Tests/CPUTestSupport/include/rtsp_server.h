/* A tiny RTSP server on the loopback interface for the streaming tests:
 * one client, DESCRIBE answered with a session description, SETUP with
 * the interleaved channels asked for, PLAY with the RTP packets given
 * (already '$'-framed), PAUSE and TEARDOWN with OK. */
#ifndef RTSP_SERVER_H
#define RTSP_SERVER_H

#include <stddef.h>
#include <stdint.h>

/* Started on a free port (returned; 0 on failure). */
int rtsp_server_start(const char *sdp, const uint8_t *frames, size_t frames_len);
/* The methods the client has sent, space-separated. */
void rtsp_server_methods(char *out, size_t n);
void rtsp_server_stop(void);

#endif
