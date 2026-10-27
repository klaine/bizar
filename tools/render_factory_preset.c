/*
 * render_factory_preset.c -- render factory presets to WAV for listening.
 *
 * Basses (BA) play a one-bar eighth-note riff around C1 at 120 BPM, then a
 * held note that slides up a fifth (so glide shows). Everything else holds a
 * C3 minor triad, or a single C3 for leads and the arpeggios, for four
 * seconds. Each file gets two extra seconds for the release and effects.
 *
 * Usage: render_factory_preset OUT_DIR [INDEX ...]
 *        (no INDEX renders the whole bank)
 */
#include "plugin_api_v2_compat.h"
#include "factory_bank.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 44100
#define BLOCK 128
#define WARMUP_BLOCKS (3 * 44100 / 2 / BLOCK)
/* One eighth note at 120 BPM. */
#define EIGHTH (RATE / 4)

typedef struct {
    int at;     /* sample offset */
    int on;
    int note;
} event_t;

static void put16(FILE *f, unsigned v) { fputc((int)(v & 255), f); fputc((int)(v >> 8 & 255), f); }
static void put32(FILE *f, unsigned v) { put16(f, v & 65535); put16(f, v >> 16); }

static int bass_events(event_t *e) {
    /* C1 C1 C2 C1 Eb1 C1 G0 Bb0, 70 % gate, then C1 held and G1 tied over it. */
    static const int riff[8] = {36, 36, 48, 36, 39, 36, 31, 34};
    int n = 0;
    for (int bar = 0; bar < 2; ++bar)
        for (int i = 0; i < 8; ++i) {
            int start = (bar * 8 + i) * EIGHTH;
            e[n++] = (event_t){start, 1, riff[i]};
            e[n++] = (event_t){start + EIGHTH * 7 / 10, 0, riff[i]};
        }
    int t = 16 * EIGHTH;
    e[n++] = (event_t){t, 1, 36};
    e[n++] = (event_t){t + 4 * EIGHTH, 1, 43};
    e[n++] = (event_t){t + 4 * EIGHTH + 10, 0, 36};
    e[n++] = (event_t){t + 8 * EIGHTH, 0, 43};
    return n;
}

static int held_events(event_t *e, int chord) {
    static const int triad[3] = {48, 51, 55};
    int count = chord ? 3 : 1, n = 0;
    for (int i = 0; i < count; ++i) e[n++] = (event_t){0, 1, chord ? triad[i] : 60};
    for (int i = 0; i < count; ++i) e[n++] = (event_t){4 * RATE, 0, chord ? triad[i] : 60};
    return n;
}

static int render(plugin_api_v2_t *api, int index, const char *dir) {
    void *instance = api->create_instance("", NULL);
    if (!instance) return 0;
    char text[16], name[64], path[512];
    snprintf(text, sizeof(text), "%d", index);
    api->set_param(instance, "preset", text);
    api->get_param(instance, "preset_name", name, (int)sizeof(name));

    /* A fresh instance keeps its effects bypassed while it clears their delay
     * memory, so play nothing until they are live, as on Move. */
    for (int b = 0; b < WARMUP_BLOCKS; ++b) {
        int16_t silence[BLOCK * 2];
        api->render_block(instance, silence, BLOCK);
    }

    event_t events[64];
    int count;
    if (strncmp(name, "BA", 2) == 0) count = bass_events(events);
    else count = held_events(events, strncmp(name, "LD", 2) != 0 && strncmp(name, "AR", 2) != 0
                                     && strncmp(name, "FX", 2) != 0);
    int last = 0;
    for (int i = 0; i < count; ++i) if (events[i].at > last) last = events[i].at;
    int blocks = (last + 2 * RATE) / BLOCK + 1;

    snprintf(path, sizeof(path), "%s/%03d %s.wav", dir, index, name);
    FILE *f = fopen(path, "wb");
    if (!f) { api->destroy_instance(instance); return 0; }
    unsigned bytes = (unsigned)blocks * BLOCK * 4;
    fwrite("RIFF", 1, 4, f); put32(f, 36 + bytes); fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put16(f, 1); put16(f, 2); put32(f, RATE); put32(f, RATE * 4);
    put16(f, 4); put16(f, 16); fwrite("data", 1, 4, f); put32(f, bytes);

    int16_t block[BLOCK * 2];
    for (int b = 0; b < blocks; ++b) {
        int start = b * BLOCK;
        for (int i = 0; i < count; ++i) {
            if (events[i].at < start || events[i].at >= start + BLOCK) continue;
            uint8_t msg[3] = {(uint8_t)(events[i].on ? 0x90 : 0x80), (uint8_t)events[i].note,
                              (uint8_t)(events[i].on ? 100 : 0)};
            api->on_midi(instance, msg, 3, 0);
        }
        api->render_block(instance, block, BLOCK);
        for (int i = 0; i < BLOCK * 2; ++i) put16(f, (uint16_t)block[i]);
    }
    fclose(f);
    api->destroy_instance(instance);
    printf("%s\n", path);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: render_factory_preset OUT_DIR [INDEX ...]\n");
        return 2;
    }
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    if (!api) return 1;
    if (argc == 2) {
        for (int i = 0; i < SF_ORIGINAL_PRESET_COUNT; ++i)
            if (!render(api, i, argv[1])) return 1;
    }
    for (int a = 2; a < argc; ++a)
        if (!render(api, atoi(argv[a]), argv[1])) return 1;
    return 0;
}
