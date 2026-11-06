/*
 * The record sound backend (--sound record:FILE): what the Sound Manager
 * and the music players give the host, written to a file a line each and
 * played nowhere, so a headless run has a host to hear its sound (the
 * channels then keep what the host has not played, and a resumed run
 * gives it again), and two runs can be compared.  Each line starts with
 * the machine's time in microseconds:
 *
 *   T pcm ID FRAMES CHANNELS RATE VOLUME HASH   16-bit samples (FNV-1a)
 *   T stop ID
 *   T midi ID N DELAY:STATUS:DATA1:DATA2 ...    N messages
 *   T silence ID                                 a synthesizer's notes off
 *   T hibernated                                 the snapshot was written
 *   T resumed                                    this process put one back
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "tb_internal.h"

BB_HOST static FILE *out;

static unsigned long long micros(void) { return (unsigned long long)(machine_time_ns() / 1000u); }

static void record_pcm(int id, const int16_t *pcm, int frames, int channels, double rate, float volume, void *ctx)
{
    (void)ctx;
    if (!pcm) {
        fprintf(out, "%llu stop %d\n", micros(), id);
        return;
    }
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < (size_t)frames * (size_t)channels; i++) {
        h = (h ^ (uint8_t)pcm[i]) * 16777619u;
        h = (h ^ (uint8_t)((uint16_t)pcm[i] >> 8)) * 16777619u;
    }
    fprintf(out, "%llu pcm %d %d %d %.1f %.4f %08x\n", micros(), id, frames, channels, rate, volume, h);
}

static void record_midi(int id, const machine_midi_event *events, int n, void *ctx)
{
    (void)ctx;
    if (!n) {
        fprintf(out, "%llu silence %d\n", micros(), id);
        return;
    }
    fprintf(out, "%llu midi %d %d", micros(), id, n);
    for (int i = 0; i < n; i++)
        fprintf(out, " %d:%02x:%02x:%02x", events[i].delay_ms, events[i].status, events[i].data1, events[i].data2);
    fputc('\n', out);
}

bool machine_set_sound_record(const char *path)
{
    if (out && out != stderr)
        fclose(out);
    out = !strcmp(path, "-") ? stderr : fopen(path, "w");
    if (!out)
        return false;
    setvbuf(out, NULL, _IOLBF, 0);
    machine_set_sound_handler(record_pcm, NULL);
    machine_set_midi_handler(record_midi, NULL);
    return true;
}

void sound_record_note(const char *what)
{
    if (out)
        fprintf(out, "%llu %s\n", micros(), what);
}
