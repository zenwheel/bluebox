/* The streaming tests' RTSP server (rtsp_server.h). */
#include "rtsp_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

static int listener = -1;
static pthread_t thread;
static char *description;
static uint8_t *frames;
static size_t nframes;
static char methods[1024];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool stopping;

static void reply(int fd, int cseq, const char *extra, const char *body)
{
    char head[1024];
    int n = snprintf(head, sizeof head, "RTSP/1.0 200 OK\r\nCSeq: %d\r\n%s", cseq, extra ? extra : "");
    if (body)
        n += snprintf(head + n, sizeof head - (size_t)n, "Content-Type: application/sdp\r\nContent-Length: %zu\r\n",
                      strlen(body));
    n += snprintf(head + n, sizeof head - (size_t)n, "\r\n");
    (void)!send(fd, head, (size_t)n, 0);
    if (body)
        (void)!send(fd, body, strlen(body), 0);
}

static void serve(int fd)
{
    char buf[8192] = "";
    size_t len = 0;
    for (;;) {
        char *end;
        while (!(end = strstr(buf, "\r\n\r\n"))) {
            ssize_t r = recv(fd, buf + len, sizeof buf - 1 - len, 0);
            if (r < 0 && errno == EAGAIN && !stopping)
                continue;
            if (r <= 0)
                return;
            len += (size_t)r;
            buf[len] = 0;
        }
        size_t used = (size_t)(end + 4 - buf);
        char method[32] = "", url[512] = "";
        sscanf(buf, "%31s %511s", method, url);
        int cseq = 0;
        char *cs = strcasestr(buf, "CSeq:");
        if (cs)
            cseq = atoi(cs + 5);
        char transport[256] = "";
        char *tr = strcasestr(buf, "Transport:");
        if (tr)
            sscanf(tr + 10, " %255[^\r\n]", transport);
        pthread_mutex_lock(&lock);
        if (strlen(methods) + strlen(method) + 2 < sizeof methods) {
            if (methods[0])
                strcat(methods, " ");
            strcat(methods, method);
        }
        pthread_mutex_unlock(&lock);
        memmove(buf, buf + used, len - used);
        len -= used;
        buf[len] = 0;
        if (!strcmp(method, "DESCRIBE")) {
            char base[600];
            snprintf(base, sizeof base, "Content-Base: %s/\r\n", url);
            reply(fd, cseq, base, description);
        } else if (!strcmp(method, "SETUP")) {
            char extra[400];
            snprintf(extra, sizeof extra, "Session: 1234;timeout=60\r\nTransport: %s\r\n", transport);
            reply(fd, cseq, extra, NULL);
        } else if (!strcmp(method, "PLAY")) {
            reply(fd, cseq, "Session: 1234\r\nRange: npt=0-\r\n", NULL);
            (void)!send(fd, frames, nframes, 0);
        } else {
            reply(fd, cseq, "Session: 1234\r\n", NULL);
            if (!strcmp(method, "TEARDOWN"))
                return;
        }
    }
}

static void *run(void *arg)
{
    (void)arg;
    while (!stopping) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(listener, &r);
        struct timeval tv = { 0, 100000 };
        if (select(listener + 1, &r, NULL, NULL, &tv) <= 0)
            continue;
        int fd = accept(listener, NULL, NULL);
        if (fd < 0)
            break;
        struct timeval rt = { 0, 100000 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof rt);
        serve(fd);
        close(fd);
    }
    return NULL;
}

int rtsp_server_start(const char *sdp, const uint8_t *f, size_t n)
{
    rtsp_server_stop();
    description = strdup(sdp);
    frames = malloc(n ? n : 1);
    memcpy(frames, f, n);
    nframes = n;
    methods[0] = 0;
    stopping = false;
    listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t sl = sizeof sa;
    if (listener < 0 || bind(listener, (struct sockaddr *)&sa, sizeof sa) || listen(listener, 2) ||
        getsockname(listener, (struct sockaddr *)&sa, &sl))
        return 0;
    pthread_create(&thread, NULL, run, NULL);
    return ntohs(sa.sin_port);
}

void rtsp_server_methods(char *out, size_t n)
{
    pthread_mutex_lock(&lock);
    snprintf(out, n, "%s", methods);
    pthread_mutex_unlock(&lock);
}

void rtsp_server_stop(void)
{
    if (listener < 0)
        return;
    stopping = true;
    pthread_join(thread, NULL);
    close(listener);
    listener = -1;
    free(description);
    free(frames);
    description = NULL;
    frames = NULL;
}
