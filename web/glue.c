/* Exports for the browser host; the DSP itself is the unmodified module. */
#include <stdint.h>
#include "plugin_api_v2_compat.h"

float sf_web_tempo = 120.0f;

__attribute__((export_name("sf_web_set_tempo")))
void sf_web_set_tempo(float bpm) { if (bpm > 0.0f) sf_web_tempo = bpm; }

__attribute__((export_name("sf_web_api")))
plugin_api_v2_t *sf_web_api(void) { return move_plugin_init_v2(0); }
