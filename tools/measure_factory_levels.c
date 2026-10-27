/*
 * measure_factory_levels.c -- play every factory preset and report its level.
 *
 * Each preset plays one held note (C1 for basses, C4 for leads, C3 for the
 * rest) at velocity 100 for four seconds, then releases it for one more. The
 * RMS over the last two held seconds (past a slow pad's attack) and the
 * sample peak over the whole render are printed in dBFS, one tab-separated line per preset, so the output trims in
 * scripts/generate_factory_bank.py can even out the bank's loudness.
 *
 * Usage: measure_factory_levels [INDEX [KEY=VALUE ...]] > levels.tsv
 *
 * With an INDEX only that preset plays, after any KEY=VALUE overrides, which
 * is a quick way to try a change before writing it into the bank.
 */
#include "plugin_api_v2_compat.h"
#include "factory_bank.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK 128
#define WARMUP_BLOCKS (3 * 44100 / 2 / BLOCK)
#define RATE 44100
#define HELD_BLOCKS (4 * RATE / BLOCK)
#define RMS_FROM (2 * RATE / BLOCK)
#define TAIL_BLOCKS (RATE / BLOCK)

int main(int argc, char **argv) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    if (!api) return 1;
    int first = argc > 1 ? atoi(argv[1]) : 0;
    int last = argc > 1 ? first + 1 : SF_ORIGINAL_PRESET_COUNT;
    for (int index = first; index < last; ++index) {
        void *instance = api->create_instance("", NULL);
        if (!instance) return 1;
        char text[16], name[64];
        snprintf(text, sizeof(text), "%d", index);
        api->set_param(instance, "preset", text);
        api->get_param(instance, "preset_name", name, (int)sizeof(name));
        for (int arg = 2; arg < argc; ++arg) {
            char key[64];
            const char *eq = strchr(argv[arg], '=');
            if (!eq || eq - argv[arg] >= (int)sizeof(key)) continue;
            memcpy(key, argv[arg], (size_t)(eq - argv[arg]));
            key[eq - argv[arg]] = '\0';
            api->set_param(instance, key, eq + 1);
        }

        /* A fresh instance keeps its effects bypassed while it clears their
         * delay memory, so play nothing until they are live, as on Move. */
        for (int b = 0; b < WARMUP_BLOCKS; ++b) {
            int16_t silence[BLOCK * 2];
            api->render_block(instance, silence, BLOCK);
        }

        int note = 48;
        if (strncmp(name, "BA", 2) == 0) note = 24 + 12;
        else if (strncmp(name, "LD", 2) == 0) note = 60;
        uint8_t on[3] = {0x90, (uint8_t)note, 100};
        uint8_t off[3] = {0x80, (uint8_t)note, 0};
        api->on_midi(instance, on, 3, 0);

        int16_t block[BLOCK * 2];
        double sum = 0.0;
        long count = 0;
        int peak = 0;
        for (int b = 0; b < HELD_BLOCKS + TAIL_BLOCKS; ++b) {
            if (b == HELD_BLOCKS) api->on_midi(instance, off, 3, 0);
            api->render_block(instance, block, BLOCK);
            for (int i = 0; i < BLOCK * 2; ++i) {
                int v = abs((int)block[i]);
                if (v > peak) peak = v;
                if (b >= RMS_FROM && b < HELD_BLOCKS) {
                    sum += (double)block[i] * block[i];
                    ++count;
                }
            }
        }
        double rms = sqrt(sum / (double)count) / 32768.0;
        printf("%d\t%s\t%.1f\t%.1f\n", index, name,
               rms > 0.0 ? 20.0 * log10(rms) : -120.0,
               peak > 0 ? 20.0 * log10(peak / 32768.0) : -120.0);
        api->destroy_instance(instance);
    }
    return 0;
}
