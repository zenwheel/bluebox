/*
 * The record speech backend (--speech record:FILE): deterministic speech
 * for tests and oracle probes.  Each request is written to the file, one
 * line each; an utterance's words and sync commands come back as word and
 * sync callbacks in text order, then it is done, all at the next safe
 * point, with no sound and no waiting.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "tb_internal.h"

static FILE *out;

static const char *op_name(int op)
{
    switch (op) {
    case MACHINE_SPEECH_SPEAK: return "speak";
    case MACHINE_SPEECH_STOP: return "stop";
    case MACHINE_SPEECH_PAUSE: return "pause";
    case MACHINE_SPEECH_CONTINUE: return "continue";
    case MACHINE_SPEECH_DISPOSE: return "dispose";
    case MACHINE_SPEECH_PHONEMES: return "phonemes";
    default: return "?";
    }
}

/* [[sync x]]: the message, four characters or a number. */
static int32_t sync_value(const char *s, const char *end)
{
    while (s < end && isspace((unsigned char)*s))
        s++;
    if (s + 2 < end && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return (int32_t)strtoul(s + 2, NULL, 16);
    if (s < end && (*s == '\'' || *s == '"')) {
        uint32_t v = 0;
        for (int i = 1; i <= 4 && s + i < end && s[i] != '\'' && s[i] != '"'; i++)
            v = v << 8 | (uint8_t)s[i];
        return (int32_t)v;
    }
    return (int32_t)strtol(s, NULL, 0);
}

static void record(const machine_speech_request *rq, void *ctx)
{
    (void)ctx;
    if (out) {
        fprintf(out, "channel %d %s", rq->channel, op_name(rq->op));
        if (rq->op == MACHINE_SPEECH_SPEAK || rq->op == MACHINE_SPEECH_PHONEMES)
            fprintf(out, " voice %s rate %.1f pitch %.1f modulation %.1f volume %.2f%s: %s", rq->voice ? rq->voice : "-",
                    rq->rate, rq->pitch, rq->modulation, rq->volume, rq->phonemes ? " phonemes" : "",
                    rq->text ? rq->text : "");
        fputc('\n', out);
        fflush(out);
    }
    if (rq->op == MACHINE_SPEECH_PHONEMES) { /* the text, upper case: stand-in phonemes */
        if (rq->result && rq->result_size) {
            size_t n = 0;
            for (const char *s = rq->text ? rq->text : ""; *s && n + 1 < rq->result_size; s++)
                rq->result[n++] = (char)toupper((unsigned char)*s);
            rq->result[n] = 0;
        }
        return;
    }
    if (rq->op == MACHINE_SPEECH_STOP) {
        machine_speech_event(rq->channel, 0, 0, 0);
        return;
    }
    if (rq->op != MACHINE_SPEECH_SPEAK)
        return;
    const char *t = rq->text ? rq->text : "";
    size_t len = strlen(t);
    for (size_t i = 0; i < len;) {
        if (t[i] == '[' && t[i + 1] == '[') { /* an embedded command */
            const char *end = strstr(t + i, "]]");
            size_t stop = end ? (size_t)(end - t) + 2 : len;
            if (!strncmp(t + i + 2, "sync", 4))
                machine_speech_event(rq->channel, 2, sync_value(t + i + 6, t + stop - 2), 0);
            i = stop;
            continue;
        }
        if (isspace((unsigned char)t[i])) {
            i++;
            continue;
        }
        size_t w = i;
        while (w < len && !isspace((unsigned char)t[w]) && !(t[w] == '[' && t[w + 1] == '['))
            w++;
        machine_speech_event(rq->channel, 1, (int32_t)i, (int32_t)(w - i));
        i = w;
    }
    machine_speech_event(rq->channel, 0, 0, 0);
}

bool machine_set_speech_record(const char *path)
{
    if (out && out != stderr)
        fclose(out);
    out = !strcmp(path, "-") ? stderr : fopen(path, "w");
    if (!out)
        return false;
    machine_set_speech_handler(record, NULL);
    return true;
}
