#include "plugin_api_v2_compat.h"
#include "../src/modulation.h"
#include "../src/sequencer.h"
#include "../src/effects.h"
#include "../src/voice.h"
#include "factory_bank.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_value(plugin_api_v2_t *api, void *instance, const char *key,
                      char *buffer, int size) {
    int n = api->get_param(instance, key, buffer, size);
    if (n < 0) fprintf(stderr, "get_param failed for key: %s\n", key);
    assert(n >= 0);
    assert(n < size);
    return n;
}

/* Locate a chain_params entry's option list and walk it. The names themselves
 * contain no quotes, so a scan for the delimiters is enough and avoids pulling
 * a JSON parser into the test. */
static const char *find_options(const char *json, const char *key) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"key\":\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p = strstr(p, "\"options\":[");
    if (!p) return NULL;
    return p + strlen("\"options\":[");
}

static int next_option(const char **cursor, char *out, size_t out_size) {
    const char *p = *cursor;
    size_t n;
    if (!p || *p == ']' || *p == '\0') return 0;
    if (*p == '"') p++;
    /* Bound the scan to this option list: the next entry's name contains a
     * `","` of its own, and matching that would swallow the rest of the JSON. */
    const char *close = strchr(p, ']');
    if (!close) return 0;
    const char *end = strstr(p, "\",");
    if (!end || end > close) end = close;
    n = (size_t)(end - p);
    if (n > 0 && p[n - 1] == '"') n--;
    if (n >= out_size) n = out_size - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    *cursor = (*end == ']') ? end : end + 2;
    return 1;
}

static void test_api_and_param_contract(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    assert(api != NULL);
    assert(api->api_version == MOVE_PLUGIN_API_VERSION_2);
    assert(api->create_instance && api->destroy_instance && api->on_midi);
    assert(api->set_param && api->get_param && api->render_block);

    const char *defaults = "{\"attack\":1,\"output\":1,\"mix\":0.25}";
    void *instance = api->create_instance("", defaults);
    assert(instance != NULL);

    char value[65536];
    read_value(api, instance, "mix", value, sizeof(value));
    assert(strtof(value, NULL) > 0.24f && strtof(value, NULL) < 0.26f);
    api->set_param(instance, "osc2_mode", "Ring");
    read_value(api, instance, "osc2_mode", value, sizeof(value));
    assert(strcmp(value, "Ring") == 0);
    api->set_param(instance, "osc1_tune", "60");
    api->set_param(instance, "osc1_wave", "0.25");
    api->set_param(instance, "osc1_timbre", "0.7");
    read_value(api, instance, "osc1_tune", value, sizeof(value));
    assert(strtof(value, NULL) == 48.5f);
    api->set_param(instance, "osc1_tune", "-48.25");
    read_value(api, instance, "osc1_tune", value, sizeof(value));
    assert(strtof(value, NULL) == -48.25f);
    read_value(api, instance, "osc1_wave", value, sizeof(value));
    assert(strtof(value, NULL) == 0.25f);
    read_value(api, instance, "osc1_timbre", value, sizeof(value));
    assert(strtof(value, NULL) == 0.7f);
    /* Mod Quantize is per oscillator and names the manual's scales. */
    api->set_param(instance, "osc1_quantize", "Octaves");
    api->set_param(instance, "osc2_quantize", "Minor Pentatonic");
    read_value(api, instance, "osc1_quantize", value, sizeof(value));
    assert(strcmp(value, "Octaves") == 0);
    read_value(api, instance, "osc2_quantize", value, sizeof(value));
    assert(strcmp(value, "Minor Pentatonic") == 0);
    /* The grid: a column names its destination, then carries one amount per
     * source row, and a macro slot names a destination and an amount. */
    api->set_param(instance, "modcol1_dest", "Pitch");
    api->set_param(instance, "modcol1_whl", "0.5");
    api->set_param(instance, "modcol2_dest", "Wav2");
    api->set_param(instance, "macro1_dest1", "Cut");
    api->set_param(instance, "macro1_amt1", "0.25");
    api->set_param(instance, "macro1", "0.75");
    api->set_param(instance, "lfo1_shape", "Shaper");
    api->set_param(instance, "lfo1_point2", "0.85");
    read_value(api, instance, "modcol1_dest", value, sizeof(value));
    assert(strcmp(value, "Pitch") == 0);
    read_value(api, instance, "modcol1_whl", value, sizeof(value));
    assert(strtof(value, NULL) == 0.5f);
    read_value(api, instance, "modcol2_dest", value, sizeof(value));
    assert(strcmp(value, "Wav2") == 0);
    read_value(api, instance, "macro1_dest1", value, sizeof(value));
    assert(strcmp(value, "Cut") == 0);
    read_value(api, instance, "macro1_amt1", value, sizeof(value));
    assert(strtof(value, NULL) == 0.25f);
    /* The Move matrix selector writes a column index; the editor aliases map
     * that column onto the same storage the modcol keys and state use. */
    api->set_param(instance, "mod_edit_col", "11");
    read_value(api, instance, "mod_edit_col", value, sizeof(value));
    assert(strcmp(value, "11") == 0);
    api->set_param(instance, "mod_edit_dest", "Res");
    api->set_param(instance, "mod_edit_lfo2", "-0.4");
    read_value(api, instance, "modcol12_dest", value, sizeof(value));
    assert(strcmp(value, "Res") == 0);
    read_value(api, instance, "modcol12_lfo2", value, sizeof(value));
    assert(strtof(value, NULL) == -0.4f);
    read_value(api, instance, "mod_edit_dest", value, sizeof(value));
    assert(strcmp(value, "Res") == 0);
    read_value(api, instance, "mod_matrix_items", value, sizeof(value));
    assert(strstr(value, "\"index\":11") != NULL);
    assert(strstr(value, "\"label\":\"12: Res\"") != NULL);
    read_value(api, instance, "lfo1_shape", value, sizeof(value));
    assert(strcmp(value, "Shaper") == 0);
    read_value(api, instance, "lfo1_point2", value, sizeof(value));
    assert(strtof(value, NULL) == 0.85f);
    read_value(api, instance, "macro1", value, sizeof(value));
    assert(strtof(value, NULL) == 0.75f);
    read_value(api, instance, "ui_hierarchy", value, sizeof(value));
    assert(strstr(value, "\"engine1\"") != NULL);
    assert(strstr(value, "\"factory\"") != NULL);
    assert(strstr(value, "\"effects\"") != NULL);
    assert(strstr(value, "\"effect2\"") != NULL);
    assert(strstr(value, "\"effect3\"") != NULL);
    /* Osc 2's level takes its own role label, not the next printf argument
     * (it once read Mod 1's destination, so knob 8 said "Cut"). */
    {
        const char *level2 = strstr(value, "{\"key\":\"osc2_level\"");
        const char *end2 = level2 ? strchr(level2, '}') : NULL;
        const char *label2 = level2 ? strstr(level2, "\"label\":\"Volume 2") : NULL;
        assert(level2 && end2 && label2 && label2 < end2);
    }
    /* Only the Wavetable engine reads the bank, so the selector is gated on it. */
    assert(strstr(value, "\"visible_if\":{\"param\":\"osc2_mode\",\"equals\":\"Wavetable\"}") != NULL);
    /* EQ and Peak EQ controls are part of each slot's own page, gated on the
     * slot's type. The four repetitive global EQ pages are gone. */
    assert(strstr(value, "\"fx1_low\",\"fx1_mid\",\"fx1_high\",\"fx1_frequency\",\"fx1_gain\",\"fx1_width\"") != NULL);
    assert(strstr(value, "\"key\":\"fx1_low\"") != NULL);
    assert(strstr(value, "\"visible_if\":{\"param\":\"fx1_type\",\"equals\":\"3-Band EQ\"}") != NULL);
    assert(strstr(value, "\"visible_if\":{\"param\":\"fx2_type\",\"equals\":\"Peak EQ\"}") != NULL);
    assert(strstr(value, "\"eq_a\"") == NULL);
    assert(strstr(value, "\"eq_b\"") == NULL);
    assert(strstr(value, "\"peak_eq_a\"") == NULL);
    assert(strstr(value, "\"peak_eq_b\"") == NULL);
    read_value(api, instance, "chain_params", value, sizeof(value));
    assert(strstr(value, "fx3_feedback") != NULL);
    assert(strstr(value, "fx1_subtype") != NULL);
    assert(strstr(value, "fx1_frequency") != NULL);
    assert(strstr(value, "fx1_gain") != NULL);
    assert(strstr(value, "fx1_width") != NULL);
    assert(strstr(value, "lfo2_point8") != NULL);
    assert(strstr(value, "macro4") != NULL);
    assert(strstr(value, "\"key\":\"osc1_tune\",\"name\":\"Tune 1\"") != NULL);
    assert(strstr(value, "\"key\":\"osc1_wave\",\"name\":\"Wave 1\"") != NULL);
    assert(strstr(value, "\"key\":\"osc1_timbre\",\"name\":\"Timbre 1\"") != NULL);
    assert(strstr(value, "\"key\":\"osc1_shape\",\"name\":\"Shape 1\"") != NULL);
    assert(strstr(value, "\"key\":\"osc1_level\",\"name\":\"Volume 1\"") != NULL);
    assert(strstr(value, "\"key\":\"osc2_tune\",\"name\":\"Tune 2\"") != NULL);
    assert(strstr(value, "\"key\":\"osc2_wave\",\"name\":\"Wave 2\"") != NULL);
    assert(strstr(value, "\"key\":\"osc2_timbre\",\"name\":\"Timbre 2\"") != NULL);
    assert(strstr(value, "\"key\":\"osc2_shape\",\"name\":\"Shape 2\"") != NULL);
    assert(strstr(value, "\"key\":\"osc2_level\",\"name\":\"Volume 2\"") != NULL);
    assert(strstr(value, "SuperWave") != NULL);
    assert(strstr(value, "Harmo") != NULL);
    assert(strstr(value, "Formant") != NULL);
    assert(strstr(value, "Chord") != NULL);
    /* Per-type preset options are the longest with Delay's fourteen entries,
     * so keep the contract inside the 32 KiB host read buffer. */
    api->set_param(instance, "fx1_type", "Delay");
    api->set_param(instance, "fx2_type", "Delay");
    api->set_param(instance, "fx3_type", "Delay");
    read_value(api, instance, "chain_params", value, sizeof(value));
    fprintf(stderr, "chain_params delay bytes: %d\n", (int)strlen(value));
    assert(strstr(value, "Filtered Ping-Pong Sync") != NULL);
    /* Move reads chain_params into 32 KiB; past that the device's parameter
     * list breaks. Keep some headroom for new parameters. */
    assert(strlen(value) < 32768);
    assert(strstr(value, "\"description\"") == NULL);
    assert(strstr(value, "MultiFilter") != NULL);
    assert(strstr(value, "Crusher") != NULL);
    assert(strstr(value, "Super Unison") != NULL);
    assert(strstr(value, "Flanger") != NULL);
    assert(strstr(value, "\"key\":\"mix\"") == NULL);
    assert(strstr(value, "\"key\":\"cutoff\",\"name\":\"Cutoff\",\"short_name\":\"CUT\"") != NULL);
    assert(strstr(value, "\"key\":\"cutoff\",\"name\":\"Cutoff\",\"short_name\":\"CUT\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.02") != NULL);
    assert(strstr(value, "\"key\":\"cutoff_hz\"") != NULL);
    assert(strstr(value, "\"access\":\"read\"") != NULL);
    assert(strstr(value, "\"key\":\"attack\",\"name\":\"Attack\",\"type\":\"float\",\"min\":1,\"max\":50000,\"step\":1,\"knobAcceleration\":\"wide\"") != NULL);
    /* The Cycling Envelope is its own page with MiniFreak's Rise/Hold/Fall and
     * mode, and the retired rate/depth keys survive as derived legacy readouts. */
    assert(strstr(value, "\"key\":\"cycle_mode\",\"name\":\"Cycle Mode\"") != NULL);
    assert(strstr(value, "\"key\":\"cycle_rise\",\"name\":\"Cycle Rise\"") != NULL);
    assert(strstr(value, "\"key\":\"cycle_fall\",\"name\":\"Cycle Fall\"") != NULL);
    assert(strstr(value, "\"key\":\"cycle_hold\",\"name\":\"Cycle Hold/Sustain\"") != NULL);
    assert(strstr(value, "\"key\":\"cycle_amount\",\"name\":\"Cycle Amount\"") != NULL);
    assert(strstr(value, "\"key\":\"cycle_mode\",\"name\":\"Cycle Mode\",\"short_name\":\"MODE\",\"type\":\"enum\",\"options\":[\"Env\",\"Run\",\"Loop\"]") != NULL);
    /* The full copy keeps the descriptions for the browser and the reference. */
    read_value(api, instance, "chain_params_full", value, sizeof(value));
    assert(strstr(value, "\"key\":\"cutoff\",\"name\":\"Cutoff\",\"short_name\":\"CUT\",\"description\":\"SEM-inspired 12 dB/oct multimode filter cutoff. Logarithmic: 0 = 20 Hz, 1 = 18 kHz\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.02") != NULL);
    assert(strstr(value, "\"key\":\"cycle_mode\",\"name\":\"Cycle Mode\",\"short_name\":\"MODE\",\"description\":\"Env: one-shot, Hold is sustain. Run: free loop. Loop: restarts per note\",\"type\":\"enum\",\"options\":[\"Env\",\"Run\",\"Loop\"]") != NULL);
    read_value(api, instance, "ui_hierarchy", value, sizeof(value));
    assert(strstr(value, "\"cycle_env\"") != NULL);
    /* Each custom LFO curve is ONE page: eight points is exactly one grid. */
    assert(strstr(value, "\"lfo1_curve\"") != NULL);
    assert(strstr(value, "\"lfo2_curve\"") != NULL);
    assert(strstr(value, "\"lfo1_a\"") == NULL);
    assert(strstr(value, "%s") == NULL);
    assert(strstr(value, "%d") == NULL);
    assert(strstr(value, "\"modcol3\"") != NULL);
    assert(strstr(value, "\"macro4_slots\"") != NULL);
    /* The Move grid presents the matrix as a column selector plus one editor
     * page. The twelve modcol levels stay in the document (the Remote UI still
     * draws them as cards) but are gated out of the Move page walk. */
    assert(strstr(value, "\"modmatrix\"") != NULL);
    assert(strstr(value, "\"modmatrix_edit\"") != NULL);
    assert(strstr(value, "\"items_param\":\"mod_matrix_items\"") != NULL);
    assert(strstr(value, "\"navigate_to\":\"modmatrix_edit\"") != NULL);
    assert(strstr(value, "\"visible_if\":{\"param\":\"mod_edit_col\",\"gt\":999}") != NULL);
    assert(strstr(value, "\"key\":\"mod_edit_dest\"") != NULL);
    assert(strstr(value, "\"label\":\"Mod Matrix\"") != NULL);
    assert(strstr(value, "\"label\":\"Matrix Editor\"") != NULL);
    assert(strstr(value, "{\"level\":\"modcol1\"") == NULL);
    assert(strstr(value, "\"label\":\"Mod 12: ") != NULL);
    assert(strstr(value, "\"lfo1_sync\"") != NULL);
    assert(strstr(value, "\"osc_phase\"") != NULL);
    assert(strstr(value, "\"label\":\"Cycling Envelope\"") != NULL);
    /* The Envelope page carries the three stage shapes and the retrigger
     * switch; the four velocity depths get their own page. */
    assert(strstr(value, "\"env_vel\"") != NULL);
    assert(strstr(value, "\"attack_curve\",\"decay_curve\",\"release_curve\",\"env_retrig\"") != NULL);
    assert(strstr(value, "\"vel_vca\",\"vel_vcf\",\"vel_env\",\"vel_time\"") != NULL);
    api->set_param(instance, "attack_curve", "Quick");
    api->set_param(instance, "release_curve", "Percussive");
    api->set_param(instance, "env_retrig", "Env Reset");
    api->set_param(instance, "vel_vca", "0.25");
    api->set_param(instance, "vel_vcf", "0.5");
    read_value(api, instance, "attack_curve", value, sizeof(value));
    assert(strcmp(value, "Quick") == 0);
    read_value(api, instance, "env_retrig", value, sizeof(value));
    assert(strcmp(value, "Env Reset") == 0);
    read_value(api, instance, "vel_vcf", value, sizeof(value));
    assert(strtof(value, NULL) == 0.5f);
    api->set_param(instance, "cycle_mode", "Loop");
    api->set_param(instance, "cycle_rise", "2400");
    api->set_param(instance, "cycle_fall", "80");
    api->set_param(instance, "cycle_hold", "300");
    api->set_param(instance, "cycle_amount", "0.6");
    read_value(api, instance, "cycle_mode", value, sizeof(value));
    assert(strcmp(value, "Loop") == 0);
    read_value(api, instance, "cycle_rise", value, sizeof(value));
    assert(strtof(value, NULL) == 2400.0f);
    read_value(api, instance, "cycle_fall", value, sizeof(value));
    assert(strtof(value, NULL) == 80.0f);
    read_value(api, instance, "cycle_hold", value, sizeof(value));
    assert(strtof(value, NULL) == 300.0f);
    read_value(api, instance, "cycle_amount", value, sizeof(value));
    assert(strtof(value, NULL) == 0.6f);
    /* The retired keys still answer, derived from the envelope. */
    api->set_param(instance, "cycle_rate", "4");
    read_value(api, instance, "cycle_rate", value, sizeof(value));
    assert(strtof(value, NULL) > 3.9f && strtof(value, NULL) < 4.1f);
    read_value(api, instance, "cycle_rise", value, sizeof(value));
    assert(strtof(value, NULL) > 124.0f && strtof(value, NULL) < 126.0f);
    read_value(api, instance, "cycle_depth", value, sizeof(value));
    assert(strtof(value, NULL) == 0.6f);
    read_value(api, instance, "state", value, sizeof(value));
    assert(strstr(value, "\"cycle_mode\":2") != NULL);
    assert(strstr(value, "\"cycle_amount\":0.6") != NULL);
    assert(strstr(value, "\"cycle_rate\"") == NULL);
    api->set_param(instance, "cycle_mode", "Run");
    /* Hand the buffer back as chain_params: the assertions that follow were
     * written against it, and this block has re-read ui_hierarchy and state. */
    read_value(api, instance, "chain_params", value, sizeof(value));
    /* Every conversion must be substituted, and the document must reach its
     * last entry: a stray comma outside a literal once split this format
     * string, which quietly turned the tail into extra varargs. */
    assert(strstr(value, "%s") == NULL);
    /* The document is delivered whole and the host's buffer size is not
     * documented, so its size is worth watching when parameters are added. */
    fprintf(stderr, "chain_params bytes: %d\n", (int)strlen(value));
    /* The destination option list is repeated once per column and per macro
     * socket, so check it against the words the grid itself uses. */
    assert(strstr(value, "\"options\":[\"Off\",\"Pitch\",\"Cut\",\"Shp1\",\"Shp2\","
                          "\"Vol2\",\"Res\",\"Out\",\"Tim1\",\"Tim2\",\"Wav1\",\"Wav2\","
                          "\"Ptc1\",\"Ptc2\",\"Vol1\",\"VCA\",\"EnvA\",\"Gld\",\"Atk\","
                          "\"Dec\",\"Sus\",\"Rel\",\"L1Rt\",\"L2Rt\",\"L1Am\",\"L2Am\","
                          "\"CyAm\",\"F1T\",\"F1I\",\"F1A\",\"F2T\",\"F2I\",\"F2A\","
                          "\"F3T\",\"F3I\",\"F3A\",\"Mac1\",\"Mac2\",\"Mac3\",\"Mac4\","
                          "\"Rise\",\"Fall\",\"Hold\",\"M1:1\",\"M1:2\"") != NULL);
    /* Macro sockets list the macros too, so the Cycling Envelope entries after
     * them keep their indices; a socket aimed at a macro does nothing. */
    assert(strstr(value, "\"key\":\"macro1_dest1\",\"name\":\"Macro 1 Dest 1\",\"type\":\"enum\","
                          "\"options\":[\"Off\"") != NULL);
    assert(value[0] == '[' && value[strlen(value) - 1] == ']');
    assert(strstr(value, "\"key\":\"lfo1_point8\"") != NULL);
    /* The device copy leaves the hidden modcol entries out (they are 10 KB);
     * the full copy and the keys themselves keep them. */
    assert(strstr(value, "\"key\":\"modcol") == NULL);
    assert(strstr(value, "\"key\":\"macro4_amt4\"") != NULL);
    assert(strstr(value, "\"key\":\"mod1_source\"") == NULL);
    assert(strstr(value, "\"key\":\"preset\"") != NULL);
    assert(strstr(value, "\"key\":\"osc1_wave\",\"name\":\"Wave 1\",\"label\":\"Wave 1: Morph\",\"short_name\":\"MORPH\"") != NULL);
    assert(strstr(value, "\"key\":\"osc2_timbre\",\"name\":\"Timbre 2\",\"label\":\"Timbre 2: Ring Depth\",\"short_name\":\"DEPTH\"") != NULL);
    api->set_param(instance, "osc1_mode", "Formant");
    api->set_param(instance, "osc2_mode", "Chord");
    read_value(api, instance, "is_loading", value, sizeof(value));
    assert(strcmp(value, "1") == 0);
    read_value(api, instance, "chain_params", value, sizeof(value));
    assert(strstr(value, "\"key\":\"osc1_wave\",\"name\":\"Wave 1\",\"label\":\"Wave 1: Interval\",\"short_name\":\"INTRV\"") != NULL);
    assert(strstr(value, "\"key\":\"osc2_timbre\",\"name\":\"Timbre 2\",\"label\":\"Timbre 2: Inv/Trsp\",\"short_name\":\"INV\"") != NULL);
    read_value(api, instance, "ui_hierarchy", value, sizeof(value));
    assert(strstr(value, "\"key\":\"osc1_wave\",\"short_name\":\"INTRV\",\"label\":\"Wave 1: Interval\"") != NULL);
    assert(strstr(value, "\"key\":\"osc2_timbre\",\"short_name\":\"INV\",\"label\":\"Timbre 2: Inv/Trsp\"") != NULL);
    read_value(api, instance, "state", value, sizeof(value));
    assert(strstr(value, "\"version\":19") != NULL);
    assert(strstr(value, "\"attack_curve\":1") != NULL);
    assert(strstr(value, "\"release_curve\":1") != NULL);
    assert(strstr(value, "\"env_retrig\":0") != NULL);
    assert(strstr(value, "\"vel_vca\":0.25") != NULL);
    assert(strstr(value, "\"vel_vcf\":0.5") != NULL);
    assert(strstr(value, "\"osc1_wave\":0.25") != NULL);
    assert(strstr(value, "\"osc1_timbre\":0.7") != NULL);
    assert(strstr(value, "\"osc1_quantize\":2") != NULL);
    assert(strstr(value, "\"osc2_quantize\":9") != NULL);
    assert(strstr(value, "\"lfo1_shape\":3") != NULL);
    assert(strstr(value, "\"lfo1_point2\":0.85") != NULL);
    assert(strstr(value, "\"macro1\":0.75") != NULL);
    assert(strstr(value, "\"fx1_high\":0") != NULL);
    assert(strstr(value, "\"fx1_subtype\":0") != NULL);
    assert(strstr(value, "\"fx1_frequency\":1000") != NULL);
    read_value(api, instance, "preset_count", value, sizeof(value));
    assert(atoi(value) >= SF_ORIGINAL_PRESET_COUNT);
    /* Before any preset is chosen the instance plays the init patch, so it
     * must not report preset 0's name. */
    read_value(api, instance, "preset", value, sizeof(value));
    assert(strcmp(value, "-1") == 0);
    read_value(api, instance, "preset_name", value, sizeof(value));
    assert(strcmp(value, "Init") == 0);
    /* The factory list is Bizar's own bank plus any imported MiniFreak bank,
     * and the Remote UI builds its selector from this one array. */
    read_value(api, instance, "preset_names", value, sizeof(value));
    assert(value[0] == '[' && value[strlen(value) - 1] == ']');
    assert(strstr(value, "\"PY Init\"") != NULL);
    api->set_param(instance, "preset", "1");
    read_value(api, instance, "preset_name", value, sizeof(value));
    assert(strcmp(value, "BA Acid Squelch") == 0);
    read_value(api, instance, "voice_mode", value, sizeof(value));
    assert(strcmp(value, "Mono") == 0);

    read_value(api, instance, "state", value, sizeof(value));
    assert(strstr(value, "\"preset\":1") != NULL);
    assert(strstr(value, "\"osc1_mode\":0") != NULL);
    /* The cutoff domain is a normalised 0..1 log control. A bare write above it
     * can only be the legacy Hz form and is converted. */
    api->set_param(instance, "cutoff", "24000");
    read_value(api, instance, "cutoff", value, sizeof(value));
    assert(strtof(value, NULL) == 1.0f);
    read_value(api, instance, "cutoff_hz", value, sizeof(value));
    assert(strtof(value, NULL) > 17990.0f && strtof(value, NULL) <= 18000.0f);
    api->set_param(instance, "cutoff", "0.5");
    read_value(api, instance, "cutoff", value, sizeof(value));
    assert(strtof(value, NULL) == 0.5f);
    read_value(api, instance, "cutoff_hz", value, sizeof(value));
    /* Half way up a 900:1 log range is 600 Hz. */
    assert(strtof(value, NULL) > 590.0f && strtof(value, NULL) < 610.0f);
    /* A pre-6 state blob holds Hz and is converted from the version, not by
     * magnitude, so a low value converts exactly. */
    api->set_param(instance, "state", "{\"version\":5,\"cutoff\":1200,\"voice_mode\":2}");
    read_value(api, instance, "cutoff", value, sizeof(value));
    assert(strtof(value, NULL) > 0.599f && strtof(value, NULL) < 0.604f);
    read_value(api, instance, "cutoff_hz", value, sizeof(value));
    assert(strtof(value, NULL) > 1195.0f && strtof(value, NULL) < 1205.0f);
    api->set_param(instance, "state", "{\"version\":5,\"cutoff\":45,\"voice_mode\":2}");
    read_value(api, instance, "cutoff_hz", value, sizeof(value));
    assert(strtof(value, NULL) > 44.0f && strtof(value, NULL) < 46.0f);
    /* A v6..v8 blob holds the old 0..100 value and is scaled down to 0..1. */
    api->set_param(instance, "state", "{\"version\":6,\"cutoff\":60,\"voice_mode\":2}");
    read_value(api, instance, "cutoff", value, sizeof(value));
    assert(strtof(value, NULL) == 0.6f);
    read_value(api, instance, "voice_mode", value, sizeof(value));
    assert(strcmp(value, "Unison") == 0);
    api->set_param(instance, "glide_mode", "Rate");
    api->set_param(instance, "bend_range", "12");
    read_value(api, instance, "glide_mode", value, sizeof(value));
    assert(strcmp(value, "Rate") == 0);
    read_value(api, instance, "bend_range", value, sizeof(value));
    assert(strtof(value, NULL) == 12.0f);

    api->destroy_instance(instance);
}

static void test_audio_and_voice_lifecycle(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    void *instance = api->create_instance("", "{\"attack\":1,\"decay\":20,\"sustain\":1,\"release\":1,\"output\":1,\"osc2_level\":0,\"stereo\":0}");
    assert(instance != NULL);

    uint8_t note_on[3] = {0x90, 60, 120};
    api->on_midi(instance, note_on, 3, 0);

    int16_t block[256];
    int nonzero = 0;
    for (int block_index = 0; block_index < 5; ++block_index) {
        api->render_block(instance, block, 128);
        for (int i = 0; i < 256; ++i) {
            if (block[i] != 0) nonzero++;
        }
    }
    assert(nonzero > 100);

    char value[32];
    read_value(api, instance, "polyphony", value, sizeof(value));
    assert(atoi(value) == 1);

    for (int note = 61; note < 66; ++note) {
        uint8_t next_note[3] = {0x90, (uint8_t)note, 96};
        api->on_midi(instance, next_note, 3, 0);
    }
    read_value(api, instance, "polyphony", value, sizeof(value));
    if (atoi(value) != 6)
        fprintf(stderr, "expected six active voices; got %s\n", value);
    assert(atoi(value) == 6);

    uint8_t note_off[3] = {0x80, 60, 0};
    api->on_midi(instance, note_off, 3, 0);
    for (int i = 0; i < 5; ++i) api->render_block(instance, block, 128);
    read_value(api, instance, "polyphony", value, sizeof(value));
    assert(atoi(value) < 6);

    api->destroy_instance(instance);
}

static void test_effect_processing(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    /* Use V.Analog rather than the default BasicWaves source: BasicWaves is
     * 6 dB quieter by design (matching the MiniFreak), and the compressor
     * threshold in this test needs a full-level source to engage. */
    const char *defaults = "{\"attack\":1,\"decay\":20,\"sustain\":1,\"output\":1,"
                           "\"osc1_mode\":8,\"osc2_level\":0,\"stereo\":0}";
    void *dry = api->create_instance("", defaults);
    void *driven = api->create_instance("", defaults);
    assert(dry && driven);
    api->set_param(driven, "fx1_type", "Drive");
    api->set_param(driven, "fx1_mix", "1");
    api->set_param(driven, "fx1_feedback", "0.95");

    uint8_t note_on[3] = {0x90, 48, 120};
    api->on_midi(dry, note_on, 3, 0);
    api->on_midi(driven, note_on, 3, 0);
    int16_t dry_block[256];
    int16_t fx_block[256];
    for (int i = 0; i < 132; ++i) {
        api->render_block(dry, dry_block, 128);
        api->render_block(driven, fx_block, 128);
    }
    int differs = 0;
    for (int i = 0; i < 256; ++i)
        if (dry_block[i] != fx_block[i]) differs++;
    assert(differs > 20);

    api->set_param(driven, "fx1_type", "EQ");
    api->set_param(driven, "fx1_low", "12");
    api->set_param(driven, "fx1_mid", "-4");
    for (int i = 0; i < 16; ++i) {
        api->render_block(dry, dry_block, 128);
        api->render_block(driven, fx_block, 128);
    }
    differs = 0;
    for (int i = 0; i < 256; ++i)
        if (dry_block[i] != fx_block[i]) differs++;
    assert(differs > 20);
    char value[32];
    read_value(api, driven, "fx1_low", value, sizeof(value));
    assert(strtof(value, NULL) == 6.0f);

    const char *new_fx_types[] = {"Flanger", "Super Unison"};
    for (int type_index = 0; type_index < 2; ++type_index) {
        api->set_param(driven, "fx1_type", new_fx_types[type_index]);
        api->set_param(driven, "fx1_mix", "1");
        api->set_param(driven, "fx1_time", "100");
        api->set_param(driven, "fx1_feedback", "0.75");
        for (int i = 0; i < 12; ++i) {
            api->render_block(dry, dry_block, 128);
            api->render_block(driven, fx_block, 128);
        }
        differs = 0;
        for (int i = 0; i < 256; ++i)
            if (dry_block[i] != fx_block[i]) differs++;
        assert(differs > 20);
        read_value(api, driven, "fx1_type", value, sizeof(value));
        assert(strcmp(value, new_fx_types[type_index]) == 0);
    }

    api->set_param(driven, "fx1_type", "Peak EQ");
    api->set_param(driven, "fx1_subtype", "Default");
    api->set_param(driven, "fx1_mix", "1");
    api->set_param(driven, "fx1_frequency", "1000");
    api->set_param(driven, "fx1_gain", "6");
    api->set_param(driven, "fx1_width", "2");
    for (int i = 0; i < 12; ++i) {
        api->render_block(dry, dry_block, 128);
        api->render_block(driven, fx_block, 128);
    }
    differs = 0;
    for (int i = 0; i < 256; ++i)
        if (dry_block[i] != fx_block[i]) differs++;
    assert(differs > 20);
    read_value(api, driven, "fx1_subtype", value, sizeof(value));
    assert(strcmp(value, "Default") == 0);
    read_value(api, driven, "fx1_frequency", value, sizeof(value));
    assert(strtof(value, NULL) == 1000.0f);

    /* Delay presets are per-type now, and the Sync versions use the Time
     * knob as a subdivision selector. */
    api->set_param(driven, "fx1_type", "Delay");
    api->set_param(driven, "fx1_subtype", "Digital Sync");
    api->set_param(driven, "fx1_mix", "1");
    api->set_param(driven, "fx1_time", "140");
    api->set_param(driven, "fx1_feedback", "0.45");
    for (int i = 0; i < 16; ++i) {
        api->render_block(dry, dry_block, 128);
        api->render_block(driven, fx_block, 128);
    }
    differs = 0;
    for (int i = 0; i < 256; ++i)
        if (dry_block[i] != fx_block[i]) differs++;
    assert(differs > 20);
    read_value(api, driven, "fx1_subtype", value, sizeof(value));
    assert(strcmp(value, "Digital Sync") == 0);

    /* The Preset knob's options are per type, so a type change has to raise
     * is_loading or Move keeps planning the knob from the old list: with
     * fx1_type at Off that is the single option "Off", and the knob does
     * nothing. The host then writes the chosen option as its index. */
    for (int i = 0; i < 40; ++i) api->render_block(driven, fx_block, 128);
    /* One read drains the latch the type changes above left behind. */
    read_value(api, driven, "is_loading", value, sizeof(value));
    read_value(api, driven, "is_loading", value, sizeof(value));
    assert(strcmp(value, "0") == 0);
    api->set_param(driven, "fx1_type", "Reverb");
    read_value(api, driven, "is_loading", value, sizeof(value));
    assert(strcmp(value, "1") == 0);
    {
        static char contract[65536];
        read_value(api, driven, "chain_params", contract, sizeof(contract));
        assert(strstr(contract, "\"key\":\"fx1_subtype\",\"name\":\"FX 1 Preset\",\"type\":\"enum\","
                                "\"options\":[\"Default\",\"Long\",\"Hall\",\"Echoes\",\"Room\",\"Dark Room\"]") != NULL);
    }
    read_value(api, driven, "fx1_subtype", value, sizeof(value));
    assert(strcmp(value, "Default") == 0);
    api->set_param(driven, "fx1_subtype", "2");
    read_value(api, driven, "fx1_subtype", value, sizeof(value));
    assert(strcmp(value, "Hall") == 0);
    /* Writing the same type again is not a change and raises nothing. */
    for (int i = 0; i < 40; ++i) api->render_block(driven, fx_block, 128);
    read_value(api, driven, "is_loading", value, sizeof(value));
    assert(strcmp(value, "0") == 0);
    api->set_param(driven, "fx1_type", "2");
    read_value(api, driven, "is_loading", value, sizeof(value));
    assert(strcmp(value, "0") == 0);
    api->set_param(driven, "fx1_subtype", "Hall");
    read_value(api, driven, "fx1_subtype", value, sizeof(value));
    assert(strcmp(value, "Hall") == 0);

    api->set_param(driven, "fx1_type", "Multi Comp");
    api->set_param(driven, "fx1_subtype", "Tighter");
    api->set_param(driven, "fx1_time", "50");
    api->set_param(driven, "fx1_feedback", "0.95");
    for (int i = 0; i < 12; ++i) {
        api->render_block(dry, dry_block, 128);
        api->render_block(driven, fx_block, 128);
    }
    differs = 0;
    for (int i = 0; i < 256; ++i)
        if (dry_block[i] != fx_block[i]) differs++;
    assert(differs > 20);
    read_value(api, driven, "fx1_type", value, sizeof(value));
    assert(strcmp(value, "Multi Comp") == 0);
    char state[65536];
    read_value(api, driven, "state", state, sizeof(state));
    assert(strstr(state, "\"fx1_subtype\":4") != NULL);
    assert(strstr(state, "\"fx1_frequency\":1000") != NULL);

    /* MiniFreak BitDepth is an amount: a higher value must crush more, not
     * restore precision. Compare the same held source at opposite endpoints. */
    api->set_param(dry, "fx1_type", "Crusher");
    api->set_param(driven, "fx1_type", "Crusher");
    api->set_param(dry, "fx1_mix", "1");
    api->set_param(driven, "fx1_mix", "1");
    api->set_param(dry, "fx1_time", "200");
    api->set_param(driven, "fx1_time", "200");
    api->set_param(dry, "fx1_feedback", "0");
    api->set_param(driven, "fx1_feedback", "0.95");
    differs = 0;
    for (int i = 0; i < 16; ++i) {
        api->render_block(dry, dry_block, 128);
        api->render_block(driven, fx_block, 128);
        for (int j = 0; j < 256; ++j)
            if (dry_block[j] != fx_block[j]) differs++;
    }
    assert(differs > 100);
    api->destroy_instance(dry);
    api->destroy_instance(driven);
}

/* FX Time spans 1..1000 since state v12. Free-running Delay keeps reading it
 * as milliseconds; every other use reads a knob position, so pre-v12 values
 * are rescaled from the old 1..350 range on load. */
static void test_fx_time_range_and_migration(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    char value[64];
    void *old = api->create_instance("", "{}");
    assert(old != NULL);
    api->set_param(old, "state",
        "{\"version\":11,\"fx1_type\":3,\"fx1_time\":175,"
        "\"fx2_type\":1,\"fx2_subtype\":0,\"fx2_time\":300,"
        "\"fx3_type\":1,\"fx3_subtype\":7,\"fx3_time\":175}");
    read_value(api, old, "fx1_time", value, sizeof(value));
    assert(fabs(atof(value) - (1.0 + 174.0 * 999.0 / 349.0)) < 0.5);
    read_value(api, old, "fx2_time", value, sizeof(value));
    assert(fabs(atof(value) - 300.0) < 0.01);
    read_value(api, old, "fx3_time", value, sizeof(value));
    assert(fabs(atof(value) - (1.0 + 174.0 * 999.0 / 349.0)) < 0.5);
    api->set_param(old, "fx2_time", "900");
    read_value(api, old, "fx2_time", value, sizeof(value));
    assert(fabs(atof(value) - 900.0) < 0.01);
    api->destroy_instance(old);

    void *current = api->create_instance("", "{}");
    assert(current != NULL);
    api->set_param(current, "state", "{\"version\":12,\"fx1_type\":3,\"fx1_time\":175}");
    read_value(api, current, "fx1_time", value, sizeof(value));
    assert(fabs(atof(value) - 175.0) < 0.01);
    api->destroy_instance(current);
}

/* v13 measured the MiniFreak's envelopes and LFOs: longer ranges, its 27 LFO
 * divisions, the extra retrigger modes, and the Run/Loop meanings. Older
 * states are moved onto those without changing how they sound. */
static void test_envelope_lfo_ranges_and_migration(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    char value[64];
    void *old = api->create_instance("", "{}");
    assert(old != NULL);
    api->set_param(old, "state",
        "{\"version\":12,\"lfo1_div\":4,\"lfo2_div\":19,\"cycle_mode\":1,"
        "\"cycle_rise_curve\":1,\"decay\":1000,\"release\":500}");
    read_value(api, old, "lfo1_div", value, sizeof(value));
    assert(strcmp(value, "1/4") == 0);
    read_value(api, old, "lfo2_div", value, sizeof(value));
    assert(strcmp(value, "8 Bar D") == 0);   /* was "12 Bar" */
    /* The old gated, note-restarted Run is the new Loop. */
    read_value(api, old, "cycle_mode", value, sizeof(value));
    assert(strcmp(value, "Loop") == 0);
    read_value(api, old, "cycle_rise_curve", value, sizeof(value));
    assert(fabs(atof(value) - (0.5 + 0.5 * 4.0 / 22.0)) < 0.001);
    read_value(api, old, "decay", value, sizeof(value));
    assert(fabs(atof(value) - 1381.55) < 0.5);
    read_value(api, old, "release", value, sizeof(value));
    assert(fabs(atof(value) - 690.78) < 0.5);
    api->destroy_instance(old);

    void *current = api->create_instance("", "{}");
    assert(current != NULL);
    api->set_param(current, "state",
        "{\"version\":13,\"lfo1_div\":16,\"cycle_mode\":1,\"decay\":1000,\"lfo1_retrig\":4}");
    read_value(api, current, "lfo1_div", value, sizeof(value));
    assert(strcmp(value, "1/4") == 0);
    read_value(api, current, "cycle_mode", value, sizeof(value));
    assert(strcmp(value, "Run") == 0);
    read_value(api, current, "decay", value, sizeof(value));
    assert(fabs(atof(value) - 1000.0) < 0.01);
    read_value(api, current, "lfo1_retrig", value, sizeof(value));
    assert(strcmp(value, "One") == 0);
    api->set_param(current, "lfo2_retrig", "Legato Kbd");
    read_value(api, current, "lfo2_retrig", value, sizeof(value));
    assert(strcmp(value, "Legato Kbd") == 0);
    api->set_param(current, "lfo1_rate", "100");
    read_value(api, current, "lfo1_rate", value, sizeof(value));
    assert(fabs(atof(value) - 100.0) < 0.01);
    api->set_param(current, "release", "46000");
    read_value(api, current, "release", value, sizeof(value));
    assert(fabs(atof(value) - 46000.0) < 0.5);
    api->destroy_instance(current);
}

static long long block_energy(const int16_t *block, int count);

/* MiniFreak-mode Shaper: each step lasts a whole LFO period and draws its own
 * segment, and a step with on = 0 rests at 0. A rise then a rest, routed to
 * Output at 10 Hz, sounds during the first 100 ms and is silent in the next. */
static void test_shaper_minifreak_mode(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    void *inst = api->create_instance("",
        "{\"attack\":1,\"decay\":1000,\"sustain\":1,\"output\":0,\"osc1_level\":1,"
        "\"osc2_level\":0,\"stereo\":0,\"lfo1_shape\":3,\"lfo1_rate\":10,"
        "\"lfo1_retrig\":1,\"modcol1_dest\":7,\"modcol1_lfo1\":1}");
    assert(inst != NULL);
    char value[1024];
    api->set_param(inst, "lfo1_shp_mode", "1");
    api->set_param(inst, "lfo1_shp_steps", "2");
    api->set_param(inst, "lfo1_shp_amp1", "1");
    api->set_param(inst, "lfo1_shp_slope1", "0");
    api->set_param(inst, "lfo1_shp_on1", "1");
    api->set_param(inst, "lfo1_shp_on2", "0");
    read_value(api, inst, "lfo1_shp_mode", value, sizeof(value));
    assert(atoi(value) == 1);
    uint8_t note_on[3] = {0x90, 60, 110};
    int16_t block[256];
    api->on_midi(inst, note_on, 3, 0);
    long long rise = 0, rest = 0;
    /* 128-frame blocks are 2.9 ms: the first step is blocks 0-33. */
    for (int b = 0; b < 68; ++b) {
        api->render_block(inst, block, 128);
        if (b >= 10 && b < 32) rise += block_energy(block, 256);
        if (b >= 38 && b < 66) rest += block_energy(block, 256);
    }
    assert(rise > 0);
    assert(rest == 0);
    api->destroy_instance(inst);
}

/* A positive VCA route opens the VCA with its source (gain 1 - a + a * src),
 * as on MiniFreak V, instead of boosting it: Velocity -> VCA at +1 makes the
 * level follow velocity, and a full-velocity note is no louder than unrouted. */
static long long vca_route_energy_from(int velocity, const char *amount, const char *source) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    void *inst = api->create_instance("",
        "{\"attack\":1,\"decay\":1000,\"sustain\":1,\"osc1_level\":1,\"osc2_level\":0,"
        "\"stereo\":0,\"vel_vca\":0,\"modcol1_dest\":15}");
    assert(inst != NULL);
    api->set_param(inst, "modcol1_vel", amount);
    api->set_param(inst, "velo_source", source);
    uint8_t note_on[3] = {0x90, 60, (uint8_t)velocity};
    int16_t block[256];
    api->on_midi(inst, note_on, 3, 0);
    long long energy = 0;
    for (int b = 0; b < 40; ++b) {
        api->render_block(inst, block, 128);
        if (b >= 8) energy += block_energy(block, 256);
    }
    api->destroy_instance(inst);
    return energy;
}

static long long vca_route_energy(int velocity, const char *amount) {
    return vca_route_energy_from(velocity, amount, "Velo + AT");
}

static void test_vca_route_law(void) {
    long long open = vca_route_energy(127, "0");
    long long full = vca_route_energy(127, "1");
    long long soft = vca_route_energy(32, "1");
    long long closed = vca_route_energy(127, "-1");
    assert(full <= open + open / 20 && full >= open - open / 20);
    /* Velocity 32 -> gain 0.25 (block_energy sums magnitudes). */
    assert(soft * 3 < full && soft * 5 > full);
    assert(closed < open / 1000);
    /* With the row set to aftertouch only, a played note reads 0 there, so a
     * full positive route closes the VCA (MiniFreak V's Velo/AT = AT). */
    assert(vca_route_energy_from(127, "1", "AT") < open / 1000);
    long long velo = vca_route_energy_from(127, "1", "Velo");
    assert(velo <= open + open / 20 && velo >= open - open / 20);
}

/* Re-striking a chord over release tails: the new notes take the released
 * voices (quietest first) and never each other, even though a voice started
 * a moment ago still sits at the bottom of its attack. */
static void test_voice_steal_order(void) {
    sf_voice_t voices[SF_MAX_VOICES];
    memset(voices, 0, sizeof(voices));
    for (int i = 0; i < 4; ++i) {
        voices[i].active = 1;
        voices[i].env_stage = ENV_RELEASE;
        voices[i].env = 0.6f;
        voices[i].last_env_out = 0.6f - 0.1f * (float)i;
        voices[i].age = (unsigned)(i + 1);
    }
    int taken[SF_MAX_VOICES] = {0};
    for (int n = 0; n < 4; ++n) {
        sf_voice_t *v = sf_voice_allocate(voices);
        int index = (int)(v - voices);
        assert(!taken[index]);
        taken[index] = 1;
        if (!v->active) {
            v->active = 1;
            v->env_stage = ENV_ATTACK;
            v->env = 0.0f;
            v->last_env_out = 0.0f;
            v->age = (unsigned)(10 + n);
        } else {
            v->steal_pending = 1;   /* fading out for the new note */
        }
    }
    /* Two free voices, then the two quietest released ones. */
    assert(taken[4] && taken[5] && taken[3] && taken[2]);
}

/* Destination scales measured on MiniFreak V: LFO/CycEnv amplitude routes
 * follow the VCA's gain law, effect routes move the knob position, and pitch
 * amounts from before v14 shrink to keep their 12-semitone depth. */
static void test_destination_scales(void) {
    int dest[SF_MOD_COLUMNS] = {DEST_LFO1_AMP, DEST_CYCLE_AMP};
    float amount[SF_MOD_COLUMNS * MODSRC_COUNT] = {0};
    int macro_dest[SF_MACROS * SF_MACRO_SLOTS] = {0};
    float macro_amount[SF_MACROS * SF_MACRO_SLOTS] = {0};
    float source[MODSRC_COUNT] = {0};
    float macros[SF_MACROS] = {0};
    amount[MODSRC_WHEEL] = 1.0f;
    amount[MODSRC_COUNT + MODSRC_WHEEL] = -0.5f;
    source[MODSRC_WHEEL] = 0.5f;
    sf_mod_input_t in = {dest, amount, macro_dest, macro_amount, source, macros, NULL, NULL, 0};
    sf_mod_values_t out;
    sf_mod_apply(&in, &out);
    assert(fabsf(out.dest[DEST_LFO1_AMP] + 0.5f) < 1e-6f);
    assert(fabsf(out.dest[DEST_CYCLE_AMP] + 0.25f) < 1e-6f);

    assert(fabsf(sf_effect_mod_time(FX_CHORUS, 0, 500.5f, 0.2f) - 700.3f) < 0.01f);
    assert(fabsf(sf_effect_mod_time(FX_DELAY, 0, 184.1f, 0.2f) - 376.9f) < 0.01f);
    assert(fabsf(sf_effect_mod_mix(FX_DELAY, 0.5f * sqrtf(0.221f), 0.25f)
                 - 0.5f * sqrtf(0.49f)) < 1e-4f);

    /* CycEnv Hold route: in Env mode Hold is the sustain level, so a full
     * Wheel route lifts a Hold of 0 to 1 and the CycEnv-gated Output stays
     * open after Fall. */
    long long held[2];
    for (int routed = 0; routed < 2; ++routed) {
        plugin_api_v2_t *api = move_plugin_init_v2(NULL);
        void *inst = api->create_instance("",
            "{\"attack\":1,\"decay\":1000,\"sustain\":1,\"output\":0,\"osc1_level\":1,"
            "\"osc2_level\":0,\"stereo\":0,\"cycle_mode\":0,\"cycle_rise\":1,"
            "\"cycle_fall\":10,\"cycle_hold\":0,\"modcol1_dest\":7,\"modcol1_cyc\":1,"
            "\"modcol2_dest\":42}");
        api->set_param(inst, "modcol2_whl", routed ? "1" : "0");
        uint8_t wheel[3] = {0xB0, 1, 127};
        uint8_t note_on[3] = {0x90, 60, 110};
        int16_t block[256];
        api->on_midi(inst, wheel, 3, 0);
        api->on_midi(inst, note_on, 3, 0);
        held[routed] = 0;
        for (int b = 0; b < 80; ++b) {
            api->render_block(inst, block, 128);
            if (b >= 60) held[routed] += block_energy(block, 256);
        }
        api->destroy_instance(inst);
    }
    assert(held[0] * 100 < held[1]);

    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    void *inst = api->create_instance("", "{}");
    api->set_param(inst, "state", "{\"version\":13,\"modcol1_dest\":1,\"modcol1_whl\":0.5}");
    char value[64];
    read_value(api, inst, "modcol1_whl", value, sizeof(value));
    assert(fabs(atof(value) - 0.1) < 1e-4);
    api->destroy_instance(inst);
}

/* One mode plays a single LFO cycle per note and then holds its end value:
 * a Square routed to Output gates the note for half a cycle, then rests. */
static void test_lfo_one_shot(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    void *inst = api->create_instance("",
        "{\"attack\":1,\"decay\":1000,\"sustain\":1,\"output\":0,\"osc1_level\":1,"
        "\"osc2_level\":0,\"stereo\":0,\"lfo1_shape\":2,\"lfo1_rate\":10,"
        "\"lfo1_retrig\":4,\"modcol1_dest\":7,\"modcol1_lfo1\":1}");
    assert(inst != NULL);
    uint8_t note_on[3] = {0x90, 60, 110};
    int16_t block[256];
    api->on_midi(inst, note_on, 3, 0);
    long long first = 0, later = 0;
    /* 10 Hz: the gate is open for the first 50 ms (17 blocks of 128). */
    for (int b = 0; b < 120; ++b) {
        api->render_block(inst, block, 128);
        if (b >= 4 && b < 14) first += block_energy(block, 256);
        if (b >= 60) later += block_energy(block, 256);
    }
    assert(first > 0);
    assert(later == 0);
    api->destroy_instance(inst);
}

static void test_wavetable_modes(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    const char *defaults = "{\"attack\":1,\"output\":1,\"osc1_level\":1,\"osc2_level\":0,\"mix\":0,\"stereo\":0}";
    void *engine1 = api->create_instance("", defaults);
    assert(engine1 != NULL);
    api->set_param(engine1, "osc1_mode", "Wavetable");
    api->set_param(engine1, "osc1_table", "Bell 03");
    api->set_param(engine1, "osc1_position", "0.8");
    uint8_t note_on[3] = {0x90, 64, 110};
    api->on_midi(engine1, note_on, 3, 0);
    int16_t block[256];
    int nonzero = 0;
    for (int i = 0; i < 4; ++i) {
        api->render_block(engine1, block, 128);
        for (int j = 0; j < 256; ++j) nonzero += block[j] != 0;
    }
    assert(nonzero > 100);
    char value[64];
    read_value(api, engine1, "osc1_mode", value, sizeof(value));
    assert(strcmp(value, "Wavetable") == 0);
    read_value(api, engine1, "osc1_table", value, sizeof(value));
    assert(strcmp(value, "Bell 03") == 0);
    api->destroy_instance(engine1);

    const char *engine2_defaults = "{\"attack\":1,\"output\":1,\"osc1_level\":0,\"osc2_level\":1,\"mix\":1,\"stereo\":0}";
    void *engine2 = api->create_instance("", engine2_defaults);
    assert(engine2 != NULL);
    api->set_param(engine2, "osc2_mode", "Wavetable");
    api->set_param(engine2, "osc2_table", "VoxSynth");
    api->set_param(engine2, "osc2_position", "0.7");
    api->on_midi(engine2, note_on, 3, 0);
    nonzero = 0;
    for (int i = 0; i < 4; ++i) {
        api->render_block(engine2, block, 128);
        for (int j = 0; j < 256; ++j) nonzero += block[j] != 0;
    }
    assert(nonzero > 100);
    read_value(api, engine2, "osc2_mode", value, sizeof(value));
    assert(strcmp(value, "Wavetable") == 0);
    read_value(api, engine2, "osc2_table", value, sizeof(value));
    assert(strcmp(value, "VoxSynth") == 0);
    api->set_param(engine2, "osc2_table", "Sohler 43");
    read_value(api, engine2, "osc2_table", value, sizeof(value));
    assert(strcmp(value, "Sohler 43") == 0);
    /* The bank is its own selector: position is Wave 2 and does not change it. */
    api->set_param(engine2, "osc2_wave", "0.2");
    read_value(api, engine2, "osc2_table", value, sizeof(value));
    assert(strcmp(value, "Sohler 43") == 0);
    read_value(api, engine2, "osc2_position", value, sizeof(value));
    assert(fabsf(strtof(value, NULL) - 0.2f) < 0.001f);
    {
        /* State round trip keeps the bank (version 17). */
        char saved[16384];
        read_value(api, engine2, "state", saved, sizeof(saved));
        void *copy = api->create_instance("", "{}");
        api->set_param(copy, "state", saved);
        read_value(api, copy, "osc2_table", value, sizeof(value));
        assert(strcmp(value, "Sohler 43") == 0);
        api->destroy_instance(copy);
    }
    {
        /* Pre-17 state: Wave was a bank crossfade and Timbre the position. */
        void *old = api->create_instance("", "{}");
        api->set_param(old, "state",
            "{\"version\":16,\"osc2_mode\":5,\"osc2_wave\":0.2,\"osc2_timbre\":0.6,\"osc2_shape\":0.9}");
        read_value(api, old, "osc2_table", value, sizeof(value));
        assert(strcmp(value, "VoxSynth") == 0); /* round(0.2 * 15) = bank 3 */
        read_value(api, old, "osc2_position", value, sizeof(value));
        assert(fabsf(strtof(value, NULL) - 0.6f) < 0.001f);
        api->destroy_instance(old);
    }
    {
        /* Pre-18 state: Additive (Osc 1 = 5, Osc 2 = 6) is gone; it becomes
         * Harmo and every later type moves down one slot. */
        void *old = api->create_instance("", "{}");
        api->set_param(old, "state", "{\"version\":17,\"osc1_mode\":5,\"osc2_mode\":6}");
        read_value(api, old, "osc1_mode", value, sizeof(value));
        assert(strcmp(value, "Harmo") == 0);
        read_value(api, old, "osc2_mode", value, sizeof(value));
        assert(strcmp(value, "Harmo") == 0);
        api->set_param(old, "state", "{\"version\":17,\"osc1_mode\":9,\"osc2_mode\":35}");
        read_value(api, old, "osc1_mode", value, sizeof(value));
        assert(strcmp(value, "V.Analog") == 0);
        read_value(api, old, "osc2_mode", value, sizeof(value));
        assert(strcmp(value, "FM / RM") == 0);
        api->destroy_instance(old);
    }
    {
        /* Pre-19 state: Cross FM (Osc 2 index 2) is gone and becomes FM / RM. */
        void *old = api->create_instance("", "{}");
        api->set_param(old, "state", "{\"version\":18,\"osc2_mode\":2}");
        read_value(api, old, "osc2_mode", value, sizeof(value));
        assert(strcmp(value, "FM / RM") == 0);
        api->set_param(old, "state", "{\"version\":18,\"osc2_mode\":3}");
        read_value(api, old, "osc2_mode", value, sizeof(value));
        assert(strcmp(value, "Wavefolder") == 0);
        api->destroy_instance(old);
    }
    api->destroy_instance(engine2);
}

static void test_legacy_oscillator_state_migration(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    const char *legacy = "{\"osc1_level\":0.8,\"osc2_level\":0.6,\"osc1_table\":2,"
                         "\"osc1_position\":0.75,\"mix\":0.25}";
    void *instance = api->create_instance("", legacy);
    assert(instance != NULL);

    char value[65536];
    read_value(api, instance, "osc1_table", value, sizeof(value));
    assert(strcmp(value, "Bell 03") == 0);
    read_value(api, instance, "osc1_wave", value, sizeof(value));
    assert(strtof(value, NULL) == 0.75f);
    read_value(api, instance, "osc1_timbre", value, sizeof(value));
    assert(strtof(value, NULL) > 0.34f && strtof(value, NULL) < 0.36f);
    read_value(api, instance, "osc1_level", value, sizeof(value));
    assert(strtof(value, NULL) > 0.59f && strtof(value, NULL) < 0.61f);
    read_value(api, instance, "osc2_level", value, sizeof(value));
    assert(strtof(value, NULL) > 0.14f && strtof(value, NULL) < 0.16f);
    read_value(api, instance, "state", value, sizeof(value));
    assert(strstr(value, "\"version\":19") != NULL);
    assert(strstr(value, "\"mix\"") == NULL);

    api->destroy_instance(instance);
}

static void test_minifreak_style_engine_modes(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    const char *defaults = "{\"attack\":1,\"decay\":20,\"sustain\":1,\"output\":1,"
                          "\"osc1_level\":1,\"osc2_level\":0,\"stereo\":0}";
    const char *types[] = {"SuperWave", "Harmo", "Formant"};
    char value[64];
    int16_t block[256];
    uint8_t note_on[3] = {0x90, 60, 110};

    for (int i = 0; i < 3; ++i) {
        void *instance = api->create_instance("", defaults);
        assert(instance != NULL);
        api->set_param(instance, "osc1_mode", types[i]);
        api->set_param(instance, "osc1_wave", "0.63");
        api->set_param(instance, "osc1_timbre", "0.72");
        api->set_param(instance, "osc1_shape", "0.8");
        api->on_midi(instance, note_on, 3, 0);
        int nonzero = 0;
        for (int block_index = 0; block_index < 4; ++block_index) {
            api->render_block(instance, block, 128);
            for (int sample = 0; sample < 256; ++sample) nonzero += block[sample] != 0;
        }
        assert(nonzero > 100);
        read_value(api, instance, "osc1_mode", value, sizeof(value));
        assert(strcmp(value, types[i]) == 0);
        api->destroy_instance(instance);
    }

    const char *engine2_defaults = "{\"attack\":1,\"output\":1,\"osc1_level\":1,"
                                   "\"osc2_level\":1,\"stereo\":0}";
    const char *engine2_types[] = {"SuperWave", "Harmo", "Chord", "MultiFilter", "Crusher",
                                   "Comb Filter", "Surgeon Filter", "Phaser Filter",
                                   "Destroy", "Formant", "Two Op FM", "FM / RM"};
    for (int i = 0; i < 12; ++i) {
        void *instance = api->create_instance("", engine2_defaults);
        assert(instance != NULL);
        api->set_param(instance, "osc2_mode", engine2_types[i]);
        api->set_param(instance, "osc2_wave", "0.55");
        api->set_param(instance, "osc2_timbre", "0.65");
        api->set_param(instance, "osc2_shape", "0.7");
        api->on_midi(instance, note_on, 3, 0);
        int nonzero = 0;
        for (int block_index = 0; block_index < 4; ++block_index) {
            api->render_block(instance, block, 128);
            for (int sample = 0; sample < 256; ++sample) nonzero += block[sample] != 0;
        }
        assert(nonzero > 100);
        read_value(api, instance, "osc2_mode", value, sizeof(value));
        assert(strcmp(value, engine2_types[i]) == 0);
        api->destroy_instance(instance);
    }
}

static void test_vanalog_timbre_is_audible_at_shape_zero(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    const char *low_defaults = "{\"attack\":1,\"decay\":100,\"sustain\":1,\"output\":1,"
                               "\"osc1_level\":1,\"osc2_level\":0,\"stereo\":0,"
                               "\"osc_phase\":0,\"osc1_wave\":0.5,\"osc1_timbre\":0.1,"
                               "\"osc1_shape\":0}";
    const char *high_defaults = "{\"attack\":1,\"decay\":100,\"sustain\":1,\"output\":1,"
                                "\"osc1_level\":1,\"osc2_level\":0,\"stereo\":0,"
                                "\"osc_phase\":0,\"osc1_wave\":0.5,\"osc1_timbre\":0.9,"
                                "\"osc1_shape\":0}";
    void *low_detune = api->create_instance("", low_defaults);
    void *high_detune = api->create_instance("", high_defaults);
    assert(low_detune && high_detune);
    api->set_param(low_detune, "osc1_mode", "V.Analog");
    api->set_param(high_detune, "osc1_mode", "V.Analog");

    uint8_t note_on[3] = {0x90, 60, 110};
    int16_t low_block[256];
    int16_t high_block[256];
    api->on_midi(low_detune, note_on, 3, 0);
    api->on_midi(high_detune, note_on, 3, 0);
    int differs = 0;
    for (int block = 0; block < 8; ++block) {
        api->render_block(low_detune, low_block, 128);
        api->render_block(high_detune, high_block, 128);
        for (int sample = 0; sample < 256; ++sample)
            if (low_block[sample] != high_block[sample]) differs++;
    }
    assert(differs > 100);

    api->destroy_instance(low_detune);
    api->destroy_instance(high_detune);

}

/* The FM / RM processor takes Osc 1's audio before Osc 1's Volume as its
 * modulator, as on the MiniFreak: with Osc 1 muted, Timbre still changes the
 * sound. */
static void test_fm_rm_modulates_from_muted_osc1(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    const char *dry = "{\"attack\":1,\"decay\":100,\"sustain\":1,\"output\":1,"
                      "\"osc1_mode\":1,\"osc1_wave\":0,\"osc1_timbre\":0,\"osc1_shape\":0.5,"
                      "\"osc1_level\":0,\"osc2_mode\":33,\"osc2_wave\":0,\"osc2_timbre\":0,"
                      "\"osc2_shape\":0,\"osc2_level\":1,\"osc2_tune\":7,\"stereo\":0,"
                      "\"osc_phase\":0}";
    void *plain = api->create_instance("", dry);
    void *fm = api->create_instance("", dry);
    assert(plain && fm);
    api->set_param(fm, "osc2_timbre", "0.05");
    char value[64];
    read_value(api, fm, "osc2_mode", value, sizeof(value));
    assert(strcmp(value, "FM / RM") == 0);
    uint8_t note_on[3] = {0x90, 48, 100};
    api->on_midi(plain, note_on, 3, 0);
    api->on_midi(fm, note_on, 3, 0);
    int16_t a[256];
    int16_t b[256];
    long diff = 0;
    long energy = 0;
    for (int block = 0; block < 16; ++block) {
        api->render_block(plain, a, 128);
        api->render_block(fm, b, 128);
        for (int i = 0; i < 256; ++i) {
            diff += labs((long)a[i] - (long)b[i]);
            energy += labs((long)a[i]);
        }
    }
    assert(energy > 0);
    assert(diff > energy / 4);
    api->destroy_instance(plain);
    api->destroy_instance(fm);
}

static void test_osc2_pitch_mod_and_chord_voicings(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    const char *defaults = "{\"attack\":1,\"decay\":100,\"sustain\":1,\"output\":1,"
                           "\"osc1_level\":0,\"osc2_level\":1,\"stereo\":0}";
    void *base_inst = api->create_instance("", defaults);
    void *mod_inst = api->create_instance("", defaults);
    assert(base_inst && mod_inst);

    api->set_param(mod_inst, "macro1_dest1", "Pitch");
    api->set_param(mod_inst, "macro1_amt1", "1.0");
    api->set_param(mod_inst, "macro1", "1.0");

    uint8_t note_on[3] = {0x90, 60, 110};
    api->on_midi(base_inst, note_on, 3, 0);
    api->on_midi(mod_inst, note_on, 3, 0);

    int16_t base_block[256];
    int16_t mod_block[256];
    int differs = 0;
    for (int b = 0; b < 4; ++b) {
        api->render_block(base_inst, base_block, 128);
        api->render_block(mod_inst, mod_block, 128);
        for (int i = 0; i < 256; ++i)
            if (base_block[i] != mod_block[i]) differs++;
    }
    assert(differs > 100);
    api->destroy_instance(base_inst);
    api->destroy_instance(mod_inst);

    /* Verify Chord engine produces distinct voicings across timbre range. */
    void *chord_a = api->create_instance("", defaults);
    void *chord_b = api->create_instance("", defaults);
    assert(chord_a && chord_b);
    api->set_param(chord_a, "osc2_mode", "Chord");
    api->set_param(chord_a, "osc2_timbre", "0.1");
    api->set_param(chord_b, "osc2_mode", "Chord");
    api->set_param(chord_b, "osc2_timbre", "0.75");
    api->on_midi(chord_a, note_on, 3, 0);
    api->on_midi(chord_b, note_on, 3, 0);
    differs = 0;
    for (int b = 0; b < 4; ++b) {
        api->render_block(chord_a, base_block, 128);
        api->render_block(chord_b, mod_block, 128);
        for (int i = 0; i < 256; ++i)
            if (base_block[i] != mod_block[i]) differs++;
    }
    assert(differs > 100);
    api->destroy_instance(chord_a);
    api->destroy_instance(chord_b);
}

static void test_mod_quantize_steps_pitch_modulation(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    const char *defaults = "{\"attack\":1,\"decay\":100,\"sustain\":1,\"output\":1,"
                           "\"osc1_level\":1,\"osc2_level\":0,\"stereo\":0,"
                           "\"lfo1_rate\":2,\"lfo1_shape\":\"Triangle\"}";
    void *smooth = api->create_instance("", defaults);
    void *quantized = api->create_instance("", defaults);
    assert(smooth && quantized);

    api->set_param(smooth, "modcol1_dest", "Ptc1");
    api->set_param(smooth, "modcol1_lfo1", "1.0");
    api->set_param(quantized, "modcol1_dest", "Ptc1");
    api->set_param(quantized, "modcol1_lfo1", "1.0");
    api->set_param(quantized, "osc1_quantize", "Octaves");

    uint8_t note_on[3] = {0x90, 60, 110};
    api->on_midi(smooth, note_on, 3, 0);
    api->on_midi(quantized, note_on, 3, 0);

    int16_t smooth_block[256];
    int16_t quantized_block[256];
    int differs = 0;
    for (int b = 0; b < 8; ++b) {
        api->render_block(smooth, smooth_block, 128);
        api->render_block(quantized, quantized_block, 128);
        for (int i = 0; i < 256; ++i)
            if (smooth_block[i] != quantized_block[i]) differs++;
    }
    assert(differs > 100);
    api->destroy_instance(smooth);
    api->destroy_instance(quantized);
}

static void test_braids_ne_engines_and_steiner_filter(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    const char *defaults = "{\"attack\":1,\"decay\":100,\"sustain\":1,\"output\":1,"
                           "\"osc1_level\":1,\"osc2_level\":0,\"stereo\":0}";
    uint8_t note_on[3] = {0x90, 55, 115};
    int16_t block_a[256];
    int16_t block_b[256];

    /* 1. Formant engine: vowel morph (/a/ vs /i/) produces distinct formant spectra. */
    void *vowel_a = api->create_instance("", defaults);
    void *vowel_i = api->create_instance("", defaults);
    assert(vowel_a && vowel_i);
    api->set_param(vowel_a, "osc1_mode", "Formant");
    api->set_param(vowel_a, "osc1_wave", "0.0");
    api->set_param(vowel_i, "osc1_mode", "Formant");
    api->set_param(vowel_i, "osc1_wave", "0.5");
    api->on_midi(vowel_a, note_on, 3, 0);
    api->on_midi(vowel_i, note_on, 3, 0);
    int differs = 0;
    for (int b = 0; b < 4; ++b) {
        api->render_block(vowel_a, block_a, 128);
        api->render_block(vowel_i, block_b, 128);
        for (int i = 0; i < 256; ++i)
            if (block_a[i] != block_b[i]) differs++;
    }
    assert(differs > 100);
    api->destroy_instance(vowel_a);
    api->destroy_instance(vowel_i);

    /* 2. Harmo engine: Shape rectifier/folder alters harmonic waveform. */
    void *harmo_clean = api->create_instance("", defaults);
    void *harmo_folded = api->create_instance("", defaults);
    assert(harmo_clean && harmo_folded);
    api->set_param(harmo_clean, "osc1_mode", "Harmo");
    api->set_param(harmo_clean, "osc1_shape", "0.0");
    api->set_param(harmo_folded, "osc1_mode", "Harmo");
    api->set_param(harmo_folded, "osc1_shape", "0.95");
    api->on_midi(harmo_clean, note_on, 3, 0);
    api->on_midi(harmo_folded, note_on, 3, 0);
    differs = 0;
    for (int b = 0; b < 4; ++b) {
        api->render_block(harmo_clean, block_a, 128);
        api->render_block(harmo_folded, block_b, 128);
        for (int i = 0; i < 256; ++i)
            if (block_a[i] != block_b[i]) differs++;
    }
    assert(differs > 100);
    api->destroy_instance(harmo_clean);
    api->destroy_instance(harmo_folded);

    /* 3. Steiner-Parker filter stability under high resonance + envelope sweep. */
    void *steiner = api->create_instance("", defaults);
    assert(steiner);
    api->set_param(steiner, "cutoff", "320");
    api->set_param(steiner, "resonance", "0.95");
    api->set_param(steiner, "filter_env", "1.0");
    api->on_midi(steiner, note_on, 3, 0);
    int nonzero = 0;
    for (int b = 0; b < 8; ++b) {
        api->render_block(steiner, block_a, 128);
        for (int i = 0; i < 256; ++i) nonzero += block_a[i] != 0;
    }
    assert(nonzero > 200);
    api->destroy_instance(steiner);
}

static void test_all_engine_modes_render(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    const char *defaults = "{\"attack\":1,\"decay\":3000,\"sustain\":1,\"release\":400,"
                           "\"output\":1,\"osc1_level\":0.9,\"osc2_level\":0.9,\"stereo\":0}";
    void *instance = api->create_instance("", defaults);
    assert(instance != NULL);

    char json[65536];
    read_value(api, instance, "chain_params", json, sizeof(json));

    uint8_t note_on[3] = {0x90, 57, 110};
    uint8_t all_off[3] = {0xB0, 123, 0};
    int16_t block[256];
    char name[64];
    int counted[2] = {0, 0};

    const char *keys[2] = {"osc1_mode", "osc2_mode"};
    for (int engine = 0; engine < 2; ++engine) {
        const char *cursor = find_options(json, keys[engine]);
        assert(cursor != NULL);
        while (next_option(&cursor, name, sizeof(name))) {
            counted[engine]++;
            api->set_param(instance, keys[engine], name);
            char readback[64];
            read_value(api, instance, keys[engine], readback, sizeof(readback));
            if (strcmp(readback, name) != 0)
                fprintf(stderr, "mode %s did not read back: %s\n", name, readback);
            assert(strcmp(readback, name) == 0);

            api->on_midi(instance, all_off, 3, 0);
            for (int i = 0; i < 6; ++i) api->render_block(instance, block, 128);
            api->on_midi(instance, note_on, 3, 0);
            int nonzero = 0;
            for (int b = 0; b < 6; ++b) {
                api->render_block(instance, block, 128);
                for (int s = 0; s < 256; ++s) nonzero += block[s] != 0;
            }
            if (nonzero <= 200)
                fprintf(stderr, "engine mode %s rendered %d nonzero samples\n", name, nonzero);
            assert(nonzero > 200);
        }
    }
    /* Every advertised mode must be reachable, including the stubs, and the two
     * engines must not have silently lost a mode. */
    assert(counted[0] >= 26);
    assert(counted[1] >= 33);

    api->destroy_instance(instance);
}

static long long block_energy(const int16_t *block, int count) {
    long long energy = 0;
    for (int i = 0; i < count; ++i) energy += block[i] < 0 ? -block[i] : block[i];
    return energy;
}

static void test_cycling_envelope(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    const char *defaults = "{\"attack\":1,\"decay\":3000,\"sustain\":1,\"output\":1,"
                           "\"osc1_level\":1,\"osc2_level\":0,\"stereo\":0,"
                           "\"cutoff\":18000,\"resonance\":0.1}";
    uint8_t note_on[3] = {0x90, 60, 110};
    uint8_t note_off[3] = {0x80, 60, 0};
    int16_t one_shot_block[256];
    int16_t runner_block[256];
    char value[64];

    void *one_shot = api->create_instance("", defaults);
    void *runner = api->create_instance("", defaults);
    assert(one_shot && runner);

    /* Route the envelope to Output so its shape is measurable as a level. The
     * base Output is deliberately part-way up so the modulation has headroom
     * and cannot saturate against the clamp. */
    api->set_param(one_shot, "cycle_mode", "Env");
    api->set_param(runner, "cycle_mode", "Run");
    /* Env mode is an ADSR: Default rise, then an exponential fall to Hold. */
    api->set_param(one_shot, "cycle_rise", "60");
    api->set_param(one_shot, "cycle_fall", "100");
    api->set_param(one_shot, "cycle_rise_curve", "0");
    api->set_param(one_shot, "cycle_fall_curve", "0");
    api->set_param(one_shot, "cycle_hold", "0.05");
    api->set_param(one_shot, "cycle_amount", "1");
    api->set_param(runner, "cycle_rise", "60");
    api->set_param(runner, "cycle_fall", "60");
    api->set_param(runner, "cycle_hold", "10");
    api->set_param(runner, "cycle_amount", "1");
    api->set_param(one_shot, "output", "0.3");
    api->set_param(runner, "output", "0.3");
    api->set_param(one_shot, "modcol1_dest", "Out");
    api->set_param(one_shot, "modcol1_cyc", "0.5");
    api->set_param(runner, "modcol1_dest", "Out");
    api->set_param(runner, "modcol1_cyc", "0.5");

    api->on_midi(one_shot, note_on, 3, 0);
    api->on_midi(runner, note_on, 3, 0);

    long long early[2] = {0, 0};
    long long peak[2] = {0, 0};
    /* Four-block windows: a single 128-frame block's energy swings with the
     * oscillator's phase by more than the envelope moves in it. */
    for (int b = 0; b < 24; ++b) {
        api->render_block(one_shot, one_shot_block, 128);
        api->render_block(runner, runner_block, 128);
        if (b >= 2 && b < 6) {
            early[0] += block_energy(one_shot_block, 256);
            early[1] += block_energy(runner_block, 256);
        } else if (b >= 18 && b < 22) {
            peak[0] += block_energy(one_shot_block, 256);
            peak[1] += block_energy(runner_block, 256);
        }
    }
    /* Both envelopes rise, so the window at the top is louder than the start. */
    if (!(peak[0] > early[0])) fprintf(stderr, "Env rise: %lld -> %lld\n", early[0], peak[0]);
    assert(peak[0] > early[0]);
    assert(peak[1] > early[1]);
    read_value(api, one_shot, "cycle_mode", value, sizeof(value));
    assert(strcmp(value, "Env") == 0);

    /* Env settles at its Sustain level while held; Run keeps cycling. */
    for (int b = 0; b < 60; ++b) {
        api->render_block(one_shot, one_shot_block, 128);
        api->render_block(runner, runner_block, 128);
    }
    long long idle = 0;
    long long running = 0;
    for (int b = 0; b < 40; ++b) {
        api->render_block(one_shot, one_shot_block, 128);
        api->render_block(runner, runner_block, 128);
        idle += block_energy(one_shot_block, 256);
        running += block_energy(runner_block, 256);
    }
    if (!(running > idle * 13 / 10))
        fprintf(stderr, "cycle Env idle %lld vs Run %lld\n", idle, running);
    assert(running > idle * 13 / 10);
    /* Env sustains far below its peak; compare averages, not totals. */
    assert(idle / 40 < peak[0] / 2);
    read_value(api, runner, "cycle_mode", value, sizeof(value));
    assert(strcmp(value, "Run") == 0);

    api->on_midi(one_shot, note_off, 3, 0);
    api->on_midi(runner, note_off, 3, 0);
    api->destroy_instance(one_shot);
    api->destroy_instance(runner);
}

static void test_cycle_matrix_route_uses_source_level(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    const char *defaults = "{\"attack\":1,\"decay\":1000,\"sustain\":1,\"output\":1,"
                           "\"osc1_level\":1,\"osc2_level\":0,\"stereo\":0,"
                           "\"osc_phase\":0,\"cycle_mode\":2,\"cycle_rise\":20,"
                           "\"cycle_fall\":20,\"cycle_amount\":1,"
                           "\"cycle_filter_direct\":0,\"osc1_wave\":0}";
    void *routed = api->create_instance("", defaults);
    void *unrouted = api->create_instance("", defaults);
    assert(routed && unrouted);

    api->set_param(routed, "modcol1_dest", "Wav1");
    api->set_param(routed, "modcol1_cyc", "1");
    api->set_param(unrouted, "modcol1_dest", "Wav1");
    api->set_param(unrouted, "modcol1_cyc", "0");

    uint8_t note_on[3] = {0x90, 60, 110};
    int16_t routed_block[256];
    int16_t unrouted_block[256];
    api->on_midi(routed, note_on, 3, 0);
    api->on_midi(unrouted, note_on, 3, 0);

    int differs = 0;
    for (int block = 0; block < 12; ++block) {
        api->render_block(routed, routed_block, 128);
        api->render_block(unrouted, unrouted_block, 128);
        for (int sample = 0; sample < 256; ++sample)
            if (routed_block[sample] != unrouted_block[sample]) differs++;
    }
    assert(differs > 100);

    api->destroy_instance(routed);
    api->destroy_instance(unrouted);
}

static void test_reverb_tail(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    /* A short pluck so the reverb's own tail is the only thing left ringing. */
    const char *shared = "{\"attack\":1,\"decay\":40,\"sustain\":0,\"release\":40,"
                         "\"output\":1,\"osc1_level\":1,\"osc2_level\":0,\"stereo\":0}";
    void *dry = api->create_instance("", shared);
    void *wet = api->create_instance("", shared);
    assert(dry && wet);
    api->set_param(wet, "fx1_type", "Reverb");
    api->set_param(wet, "fx1_mix", "0.9");
    api->set_param(wet, "fx1_time", "300");
    api->set_param(wet, "fx1_feedback", "0.1");

    uint8_t note_on[3] = {0x90, 60, 110};
    int16_t dry_block[256];
    int16_t wet_block[256];

    /* Prime both instances past the effect buffers' incremental clear. That
     * clear keeps the slot dry for the first ~128 blocks, and the pluck is only
     * ~14 blocks long, so without this the reverb never sees any input at all. */
    for (int b = 0; b < 220; ++b) {
        api->render_block(dry, dry_block, 128);
        api->render_block(wet, wet_block, 128);
    }

    api->on_midi(dry, note_on, 3, 0);
    api->on_midi(wet, note_on, 3, 0);

    /* Let the note itself die completely (sustain 0, 40 ms decay) so whatever
     * is left in the window below can only be the reverb tail. */
    for (int b = 0; b < 30; ++b) {
        api->render_block(dry, dry_block, 128);
        api->render_block(wet, wet_block, 128);
    }
    long long dry_energy = 0;
    long long wet_energy = 0;
    int differs = 0;
    for (int b = 0; b < 8; ++b) {
        api->render_block(dry, dry_block, 128);
        api->render_block(wet, wet_block, 128);
        for (int s = 0; s < 256; ++s) {
            dry_energy += dry_block[s] < 0 ? -dry_block[s] : dry_block[s];
            wet_energy += wet_block[s] < 0 ? -wet_block[s] : wet_block[s];
            if (dry_block[s] != wet_block[s]) differs++;
        }
    }
    if (wet_energy < 20000)
        fprintf(stderr, "reverb tail energy %lld vs dry %lld\n", wet_energy, dry_energy);
    assert(differs > 200);
    /* The tail is still ringing where the dry note is gone. The old four-tap
     * resonator had essentially nothing left at this point. */
    assert(wet_energy > 20000);

    /* Every subtype must stay finite and audible. */
    const char *subtypes[] = {"Default", "Long", "Hall", "Echoes", "Room",
                              "Dark Room"};
    for (int i = 0; i < 6; ++i) {
        api->set_param(wet, "fx1_subtype", subtypes[i]);
        api->on_midi(wet, note_on, 3, 0);
        long long energy = 0;
        for (int b = 0; b < 60; ++b) {
            api->render_block(wet, wet_block, 128);
            for (int s = 0; s < 256; ++s)
                energy += wet_block[s] < 0 ? -wet_block[s] : wet_block[s];
        }
        assert(energy > 0);
        char value[32];
        read_value(api, wet, "fx1_subtype", value, sizeof(value));
        assert(strcmp(value, subtypes[i]) == 0);
    }
    api->destroy_instance(dry);
    api->destroy_instance(wet);
}

static void test_voice_sum_does_not_duck(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    const char *defaults = "{\"attack\":1,\"decay\":3000,\"sustain\":1,\"output\":1,"
                           "\"osc1_level\":1,\"osc2_level\":0,\"stereo\":0,"
                           "\"cutoff\":18000,\"resonance\":0.1}";
    /* One loud note, and the same note beside five voices that are active but
     * effectively silent (velocity 1). The old gain was 0.56/sqrt(active), so
     * the loud note stepped down to 41% when the others arrived even though
     * they contributed nothing audible. That is the reported duck. */
    void *alone = api->create_instance("", defaults);
    void *crowded = api->create_instance("", defaults);
    assert(alone && crowded);

    uint8_t loud[3] = {0x90, 60, 120};
    uint8_t whisper[3] = {0x90, 0, 1};
    api->on_midi(alone, loud, 3, 0);
    api->on_midi(crowded, loud, 3, 0);
    for (int note = 61; note <= 65; ++note) {
        whisper[1] = (uint8_t)note;
        api->on_midi(crowded, whisper, 3, 0);
    }

    int16_t a_block[256];
    int16_t b_block[256];
    for (int b = 0; b < 20; ++b) {
        api->render_block(alone, a_block, 128);
        api->render_block(crowded, b_block, 128);
    }
    long long e_alone = 0;
    long long e_crowded = 0;
    for (int b = 0; b < 8; ++b) {
        api->render_block(alone, a_block, 128);
        e_alone += block_energy(a_block, 256);
        api->render_block(crowded, b_block, 128);
        e_crowded += block_energy(b_block, 256);
    }
    char value[32];
    read_value(api, crowded, "polyphony", value, sizeof(value));
    assert(atoi(value) == 6);
    if (!(e_crowded > e_alone * 9 / 10))
        fprintf(stderr, "voice sum ducked: alone %lld, crowded %lld\n", e_alone, e_crowded);
    assert(e_crowded > e_alone * 9 / 10);

    /* And a real six-voice chord is *louder* than one note rather than quiet
     * per note, which is what the fixed gain plus the soft clipper buys. */
    void *chord = api->create_instance("", defaults);
    assert(chord);
    api->on_midi(chord, loud, 3, 0);
    for (int note = 61; note <= 65; ++note) {
        whisper[1] = (uint8_t)note;
        whisper[2] = 120;
        api->on_midi(chord, whisper, 3, 0);
    }
    for (int b = 0; b < 20; ++b) api->render_block(chord, b_block, 128);
    long long e_chord = 0;
    for (int b = 0; b < 8; ++b) {
        api->render_block(chord, b_block, 128);
        e_chord += block_energy(b_block, 256);
    }
    assert(e_chord > e_alone);
    api->destroy_instance(alone);
    api->destroy_instance(crowded);
    api->destroy_instance(chord);
}

/* Every preset of Bizar's own bank loads, carries a category tag, and plays:
 * a held note (arpeggios and sequences included) must make sound without
 * overloading the output. */
static void test_factory_bank(void) {
    static const char *const tags[] = {"BA ", "LD ", "KY ", "PY ", "PD ", "PL ", "AR ", "FX "};
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    char value[256];
    int16_t block[256];
    for (int index = 0; index < SF_ORIGINAL_PRESET_COUNT; ++index) {
        void *instance = api->create_instance("", NULL);
        assert(instance);
        char text[16];
        snprintf(text, sizeof(text), "%d", index);
        api->set_param(instance, "preset", text);
        read_value(api, instance, "preset_name", value, sizeof(value));
        int tagged = 0;
        for (int t = 0; t < 8; ++t) tagged |= strncmp(value, tags[t], 3) == 0;
        if (!tagged) {
            fprintf(stderr, "factory preset %d (%s) has no category tag\n", index, value);
            assert(0);
        }
        /* Every preset has the Timbre and Space macros. */
        for (int macro = 1; macro <= 2; ++macro) {
            char key[32];
            snprintf(key, sizeof(key), "macro%d_dest1", macro);
            read_value(api, instance, key, value, sizeof(value));
            if (strcmp(value, "Off") == 0) {
                fprintf(stderr, "factory preset %d has no Macro %d\n", index, macro);
                assert(0);
            }
        }
        uint8_t on[3] = {0x90, 60, 100};
        api->on_midi(instance, on, 3, 0);
        long long energy = 0;
        int peak = 0;
        for (int b = 0; b < 700; ++b) {
            api->render_block(instance, block, 128);
            energy += block_energy(block, 256);
            for (int i = 0; i < 256; ++i) {
                int v = block[i] < 0 ? -block[i] : block[i];
                if (v > peak) peak = v;
            }
        }
        if (energy == 0 || peak > 26000) {
            fprintf(stderr, "factory preset %d: energy %lld peak %d\n", index, energy, peak);
            assert(0);
        }
        api->destroy_instance(instance);
    }
}

/* Changing preset while notes and effect tails sound must not step the
 * output: the old sound is cut and its last sample fades out. */
static void test_preset_change_declick(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    void *instance = api->create_instance("", NULL);
    assert(instance);
    int16_t block[256];
    int prev_l = 0, prev_r = 0, worst = 0;
    api->set_param(instance, "preset", "64");
    for (int b = 0; b < 520; ++b) api->render_block(instance, block, 128);
    uint8_t chord[3][3] = {{0x90, 48, 110}, {0x90, 55, 110}, {0x90, 63, 110}};
    for (int n = 0; n < 3; ++n) api->on_midi(instance, chord[n], 3, 0);
    for (int b = 0; b < 360; ++b) {
        if (b == 345) api->set_param(instance, "preset", "0");
        api->render_block(instance, block, 128);
        for (int i = 0; i < 128; ++i) {
            int dl = abs(block[2 * i] - prev_l), dr = abs(block[2 * i + 1] - prev_r);
            prev_l = block[2 * i];
            prev_r = block[2 * i + 1];
            if (b >= 345 && (dl > worst || dr > worst)) worst = dl > dr ? dl : dr;
        }
    }
    assert(worst < 200);
    api->destroy_instance(instance);
}

static void test_imported_minifreak_bank(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    void *instance = api->create_instance("", "{\"attack\":1,\"output\":1,\"stereo\":0}");
    assert(instance != NULL);

    char value[65536];
    read_value(api, instance, "preset_count", value, sizeof(value));
    int count = atoi(value);
    assert(count >= SF_ORIGINAL_PRESET_COUNT);
    if (count <= SF_ORIGINAL_PRESET_COUNT) {
        /* No bank was present at build time; nothing more to check. */
        api->destroy_instance(instance);
        return;
    }

    read_value(api, instance, "preset_names", value, sizeof(value));
    assert(strstr(value, "2Dirty2Handle") != NULL);

    /* The shipped bank's "3 Steps" patch routes CycEnv into the matrix. Its
     * source must be enabled without enabling the unrelated direct filter
     * sweep. The loader also repairs older generated bank headers. */
    char mf_index[16];
    if (count > SF_ORIGINAL_PRESET_COUNT + 1) {
        snprintf(mf_index, sizeof(mf_index), "%d", SF_ORIGINAL_PRESET_COUNT + 1);
        api->set_param(instance, "preset", mf_index);
        read_value(api, instance, "preset_name", value, sizeof(value));
        assert(strcmp(value, "3 Steps") == 0);
        read_value(api, instance, "modcol1_cyc", value, sizeof(value));
        assert(strtof(value, NULL) > 0.1f);
        read_value(api, instance, "cycle_amount", value, sizeof(value));
        assert(strtof(value, NULL) > 0.99f);
        read_value(api, instance, "cycle_filter_direct", value, sizeof(value));
        assert(strcmp(value, "0") == 0);
    }

    /* Daft Lead is shown as V.Analog in MiniFreak V. Its Osc1_Type export is
     * slot 4, which is now anchored to the V.Analog model rather than guessed
     * to be Wavetable. */
    if (count > SF_ORIGINAL_PRESET_COUNT + 47) {
        snprintf(mf_index, sizeof(mf_index), "%d", SF_ORIGINAL_PRESET_COUNT + 47);
        api->set_param(instance, "preset", mf_index);
        read_value(api, instance, "preset_name", value, sizeof(value));
        assert(strcmp(value, "Daft Lead") == 0);
        read_value(api, instance, "osc1_mode", value, sizeof(value));
        assert(strcmp(value, "V.Analog") == 0);
        read_value(api, instance, "modcol1_dest", value, sizeof(value));
        assert(strcmp(value, "Shp1") == 0);
        read_value(api, instance, "modcol1_cyc", value, sizeof(value));
        assert(strtof(value, NULL) > 0.44f && strtof(value, NULL) < 0.45f);
        read_value(api, instance, "cycle_mode", value, sizeof(value));
        assert(strcmp(value, "Env") == 0);
        read_value(api, instance, "cycle_rise", value, sizeof(value));
        assert(strtof(value, NULL) == 1.0f);
        read_value(api, instance, "cycle_fall", value, sizeof(value));
        assert(strtof(value, NULL) > 9550.0f && strtof(value, NULL) < 9580.0f);
        /* Env mode's Fall is the ADSR decay curve: the knob sits at 0.766 of
         * its travel, a 1.38 s time constant, stored as 9.56 s to -60 dB. The
         * curve knobs keep the hardware's centre value, 0.49998, which Env
         * mode reads as Default. */
        read_value(api, instance, "cycle_rise_curve", value, sizeof(value));
        assert(fabsf(strtof(value, NULL) - 0.49998f) < 0.000005f);
        read_value(api, instance, "cycle_fall_curve", value, sizeof(value));
        assert(fabsf(strtof(value, NULL) - 0.49998f) < 0.000005f);
        read_value(api, instance, "macro2", value, sizeof(value));
        assert(strtof(value, NULL) > 0.21f && strtof(value, NULL) < 0.23f);

        /* The imported one-shot CycEnv route must audibly move V.Analog's
         * pulse Shape; this catches a broken source, destination or amount scale. */
        void *static_shape = api->create_instance("", NULL);
        assert(static_shape);
        api->set_param(static_shape, "preset", mf_index);
        api->set_param(instance, "fx1_type", "Off");
        api->set_param(instance, "fx2_type", "Off");
        api->set_param(instance, "fx3_type", "Off");
        api->set_param(static_shape, "fx1_type", "Off");
        api->set_param(static_shape, "fx2_type", "Off");
        api->set_param(static_shape, "fx3_type", "Off");
        api->set_param(static_shape, "modcol1_cyc", "0");
        uint8_t daft_note[3] = {0x90, 72, 100};
        int16_t moving_block[256];
        int16_t still_block[256];
        api->on_midi(instance, daft_note, 3, 0);
        api->on_midi(static_shape, daft_note, 3, 0);
        int changed_samples = 0;
        for (int block_index = 0; block_index < 100; ++block_index) {
            api->render_block(instance, moving_block, 128);
            api->render_block(static_shape, still_block, 128);
            for (int sample = 0; sample < 256; ++sample)
                if (moving_block[sample] != still_block[sample]) changed_samples++;
        }
        assert(changed_samples > 100);
        api->destroy_instance(static_shape);
    }

    /* Every imported preset must load, name itself, and render audio. */
    int16_t block[256];
    uint8_t note_on[3] = {0x90, 55, 110};
    for (int index = SF_ORIGINAL_PRESET_COUNT; index < count; ++index) {
        api->set_param(instance, "preset", "0");
        char index_text[16];
        snprintf(index_text, sizeof(index_text), "%d", index);
        api->set_param(instance, "preset", index_text);
        char readback[16];
        read_value(api, instance, "preset", readback, sizeof(readback));
        assert(atoi(readback) == index);

        read_value(api, instance, "preset_name", value, sizeof(value));
        if (value[0] == '\0' || strcmp(value, "?") == 0) {
            fprintf(stderr, "preset %d has no name\n", index);
            assert(0);
        }

        read_value(api, instance, "state", value, sizeof(value));
        assert(strstr(value, "\"version\":19") != NULL);

        api->on_midi(instance, note_on, 3, 0);
        int nonzero = 0;
        for (int b = 0; b < 5; ++b) {
            api->render_block(instance, block, 128);
            nonzero += block_energy(block, 256) ? 1 : 0;
        }
        assert(nonzero > 0);
    }

    api->destroy_instance(instance);
}

static void test_instance_pool(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    void *instances[8];
    for (int i = 0; i < 8; ++i) {
        instances[i] = api->create_instance("", NULL);
        assert(instances[i] != NULL);
    }
    assert(api->create_instance("", NULL) == NULL);
    api->destroy_instance(instances[3]);
    instances[3] = api->create_instance("", NULL);
    assert(instances[3] != NULL);
    for (int i = 0; i < 8; ++i) api->destroy_instance(instances[i]);
}

/* The Shaper LFO: pure evaluator checks plus a state round trip. */
static void test_shaper_lfo(void) {
    sf_shaper_t shaper;
    sf_shaper_default(&shaper);
    /* Default is the original eight-point linear curve. */
    assert(fabsf(sf_mod_lfo_sample(LFO_CUSTOM, 0.0f, &shaper, NULL) - (-1.0f)) < 1e-5f);
    assert(fabsf(sf_mod_lfo_sample(LFO_CUSTOM, 0.0625f, &shaper, NULL) - (-0.75f)) < 1e-4f);
    assert(fabsf(sf_mod_lfo_sample(LFO_CUSTOM, 0.5f, &shaper, NULL) - 1.0f) < 1e-5f);
    assert(sf_shaper_is_default_extension(&shaper));

    /* Four steps, slope 0: a staircase that holds each level. */
    shaper.steps = 4;
    for (int i = 0; i < 4; ++i) { shaper.amp[i] = (float)i * 0.25f; shaper.slope[i] = 0.0f; }
    assert(fabsf(sf_mod_lfo_sample(LFO_CUSTOM, 0.30f, &shaper, NULL) - 0.25f) < 1e-5f);
    assert(fabsf(sf_mod_lfo_sample(LFO_CUSTOM, 0.49f, &shaper, NULL) - 0.25f) < 1e-5f);
    assert(fabsf(sf_mod_lfo_sample(LFO_CUSTOM, 0.99f, &shaper, NULL) - 0.75f) < 1e-5f);
    /* A muted step sits at zero. */
    shaper.on[2] = 0;
    assert(fabsf(sf_mod_lfo_sample(LFO_CUSTOM, 0.60f, &shaper, NULL)) < 1e-6f);
    /* A curve bends the glide without moving its endpoints. */
    shaper.slope[1] = 1.0f; shaper.curve[1] = 0.9f;
    float linear = 0.25f + (0.0f - 0.25f) * 0.5f;
    float bent = sf_mod_lfo_sample(LFO_CUSTOM, 0.375f, &shaper, NULL);
    assert(fabsf(bent - linear) > 0.01f);
    assert(sf_shaper_is_default_extension(&shaper) == 0);

    /* Extended keys survive a state round trip. */
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    void *a = api->create_instance(".", "{}");
    void *b = api->create_instance(".", "{}");
    assert(a && b);
    char value[64];
    static char state[65536];
    api->set_param(a, "lfo1_shp_steps", "5");
    api->set_param(a, "lfo1_shp_amp12", "0.4");
    api->set_param(a, "lfo1_shp_slope3", "0.25");
    api->set_param(a, "lfo1_shp_on2", "0");
    read_value(api, a, "lfo1_shp_steps", value, sizeof(value));
    assert(strcmp(value, "5") == 0);
    int n = api->get_param(a, "state", state, sizeof(state));
    assert(n > 0 && n < (int)sizeof(state));
    api->set_param(b, "state", state);
    read_value(api, b, "lfo1_shp_steps", value, sizeof(value));
    assert(strcmp(value, "5") == 0);
    read_value(api, b, "lfo1_shp_amp12", value, sizeof(value));
    assert(fabsf((float)atof(value) - 0.4f) < 1e-4f);
    read_value(api, b, "lfo1_shp_slope3", value, sizeof(value));
    assert(fabsf((float)atof(value) - 0.25f) < 1e-4f);
    read_value(api, b, "lfo1_shp_on2", value, sizeof(value));
    assert(strcmp(value, "0") == 0);
    api->destroy_instance(a);
    api->destroy_instance(b);
}

/* The sequencer core and its plugin integration. */
static void test_sequencer(void) {
    sf_seq_t seq;
    sf_seq_event_t ev[SF_SEQ_EVENTS];
    sf_seq_init(&seq);

    /* Division list: straight at 1/4,10 -> 1/16, dotted below, triplet above. */
    assert(fabsf(sf_seq_division_beats(10) - 0.25f) < 1e-6f);
    assert(fabsf(sf_seq_division_beats(9) - 0.375f) < 1e-6f);
    assert(fabsf(sf_seq_division_beats(11) - 0.25f * 2.0f / 3.0f) < 1e-6f);
    assert(fabsf(sf_seq_division_beats(4) - 1.0f) < 1e-6f);

    /* Pattern string round trip, with a rest and a chord. */
    sf_seq_set_data(&seq, "60.108.0,,63.90.2+67.80.0");
    assert(seq.steps[0].count == 1 && seq.steps[0].pitch[0] == 60 && seq.steps[0].velocity[0] == 108);
    assert(seq.steps[1].count == 0);
    assert(seq.steps[2].count == 2 && seq.steps[2].length[0] == 2 && seq.steps[2].pitch[1] == 67);
    seq.length = 3;
    char text[256];
    assert(sf_seq_get_data(&seq, text, sizeof(text)) > 0);
    assert(strcmp(text, "60.108.0,,63.90.2+67.80.0") == 0);

    /* Seq mode: ticks at 1/16 and 120 BPM = 5512.5 samples; transposed by the
     * held key relative to note 60; stops (with a note off) on release. */
    seq.mode = SEQ_MODE_SEQ;
    sf_seq_key_down(&seq, 62, 100);
    int ons = 0, offs = 0, first_on_note = -1;
    long first_on_at = -1, second_on_at = -1;
    for (long i = 0; i < 12000; ++i) {
        int n = sf_seq_advance(&seq, 120.0f, 44100.0f, ev, SF_SEQ_EVENTS);
        for (int e = 0; e < n; ++e) {
            if (ev[e].on) {
                ++ons;
                if (first_on_at < 0) { first_on_at = i; first_on_note = ev[e].note; }
                else if (second_on_at < 0) second_on_at = i;
            } else ++offs;
        }
    }
    assert(first_on_at == 0 && first_on_note == 62);
    assert(ons >= 2 && offs >= 1);
    assert(second_on_at > 0); /* step 2 is a rest, so the next note is the chord */
    sf_seq_key_up(&seq, 62);
    int n = sf_seq_advance(&seq, 120.0f, 44100.0f, ev, SF_SEQ_EVENTS);
    (void)n;
    assert(seq.sounding_count == 0 || seq.running == 0 || n >= 0);
    for (int i = 0; i < 4; ++i) sf_seq_advance(&seq, 120.0f, 44100.0f, ev, SF_SEQ_EVENTS);
    assert(seq.running == 0 && seq.sounding_count == 0);

    /* Arp up over two held keys and one extra octave: 60 64 72 76. */
    sf_seq_init(&seq);
    seq.mode = SEQ_MODE_ARP; seq.arp_mode = ARP_UP; seq.arp_octaves = 1;
    sf_seq_key_down(&seq, 64, 100);
    sf_seq_key_down(&seq, 60, 100);
    int expect[4] = {60, 64, 72, 76}, seen = 0;
    for (long i = 0; i < 6 * 5600 && seen < 4; ++i) {
        int m = sf_seq_advance(&seq, 120.0f, 44100.0f, ev, SF_SEQ_EVENTS);
        for (int e = 0; e < m; ++e)
            if (ev[e].on) { assert(ev[e].note == expect[seen]); ++seen; }
    }
    assert(seen == 4);

    /* Through the plugin: keys feed the sequencer, which plays voices. */
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    void *inst = api->create_instance(".", "{}");
    assert(inst);
    api->set_param(inst, "seq_data", "60.100.0,64.100.0,67.100.0,64.100.0");
    api->set_param(inst, "seq_length", "4");
    api->set_param(inst, "seq_mode", "2");
    char value[64];
    read_value(api, inst, "seq_mode", value, sizeof(value));
    assert(strcmp(value, "2") == 0);
    uint8_t note_on[3] = {0x90, 60, 100}, note_off[3] = {0x80, 60, 0};
    api->on_midi(inst, note_on, 3, 0);
    int16_t audio[128 * 2];
    long energy = 0;
    for (int b = 0; b < 200; ++b) {
        api->render_block(inst, audio, 128);
        for (int i = 0; i < 256; ++i) energy += labs(audio[i]);
    }
    assert(energy > 0);
    api->on_midi(inst, note_off, 3, 0);
    for (int b = 0; b < 400; ++b) api->render_block(inst, audio, 128);
    /* After release and the envelope tail, the sequencer is silent. */
    long tail = 0;
    for (int b = 0; b < 40; ++b) {
        api->render_block(inst, audio, 128);
        for (int i = 0; i < 256; ++i) tail += labs(audio[i]);
    }
    assert(tail == 0);

    /* State round trip keeps the pattern and mode. */
    static char state[65536];
    assert(api->get_param(inst, "state", state, sizeof(state)) > 0);
    assert(strstr(state, "\"seq_data\":\"60.100.0,64.100.0,67.100.0,64.100.0\"") != NULL);
    void *other = api->create_instance(".", "{}");
    api->set_param(other, "state", state);
    read_value(api, other, "seq_data", text, sizeof(text));
    assert(strcmp(text, "60.100.0,64.100.0,67.100.0,64.100.0") == 0);
    read_value(api, other, "seq_mode", value, sizeof(value));
    assert(strcmp(value, "2") == 0);
    api->destroy_instance(other);
    api->destroy_instance(inst);
}

/* Step automation: per-step lane offsets, stepped or smooth, reaching the
 * modulation destinations while the pattern plays, saved with the state and
 * imported from the factory bank. */
static void test_seq_automation(void) {
    sf_seq_t seq;
    sf_seq_event_t ev[SF_SEQ_EVENTS];
    char text[SF_SEQ_LANE_DATA_MAX];

    /* Parse/format: empty entries are 0, values clamp to -1..1, and trailing
     * zero steps are not written. */
    sf_seq_init(&seq);
    assert(seq.lane_smooth[0] == 1 && seq.lane_dest[0] == 0);
    sf_seq_set_lane_data(&seq, 0, "0.5,,-2,0.25,0,0");
    assert(seq.lane_value[0][0] == 0.5f && seq.lane_value[0][1] == 0.0f);
    assert(seq.lane_value[0][2] == -1.0f && seq.lane_value[0][3] == 0.25f);
    assert(sf_seq_get_lane_data(&seq, 0, text, sizeof(text)) > 0);
    assert(strcmp(text, "0.5,,-1,0.25") == 0);
    assert(sf_seq_lane_in_use(&seq, 0) && !sf_seq_lane_in_use(&seq, 1));
    assert(sf_seq_get_lane_data(&seq, 0, text, 4) == -1);

    /* A lane only plays with the pattern: off while idle, and in Arp mode. */
    sf_seq_set_lane_data(&seq, 0, "0.5,-0.5,0.25,0");
    seq.lane_dest[0] = DEST_CUTOFF;
    seq.lane_smooth[0] = 0;
    seq.length = 4;
    assert(sf_seq_lane_value(&seq, 0) == 0.0f);
    seq.mode = SEQ_MODE_ARP;
    sf_seq_key_down(&seq, 60, 100);
    sf_seq_advance(&seq, 120.0f, 44100.0f, ev, SF_SEQ_EVENTS);
    assert(sf_seq_lane_value(&seq, 0) == 0.0f);
    sf_seq_panic(&seq, ev, SF_SEQ_EVENTS);

    /* Stepped: each step holds its value; a 1/16 at 120 BPM is 5512.5
     * samples. */
    seq.mode = SEQ_MODE_SEQ;
    sf_seq_key_down(&seq, 60, 100);
    sf_seq_advance(&seq, 120.0f, 44100.0f, ev, SF_SEQ_EVENTS);
    assert(sf_seq_lane_value(&seq, 0) == 0.5f);
    for (int i = 0; i < 2756; ++i) sf_seq_advance(&seq, 120.0f, 44100.0f, ev, SF_SEQ_EVENTS);
    assert(sf_seq_lane_value(&seq, 0) == 0.5f);
    for (int i = 0; i < 2756; ++i) sf_seq_advance(&seq, 120.0f, 44100.0f, ev, SF_SEQ_EVENTS);
    assert(sf_seq_lane_value(&seq, 0) == -0.5f);
    /* Smooth: halfway through step 1 it is halfway to step 2's value, and
     * the last step glides back to the first. */
    seq.lane_smooth[0] = 1;
    for (int i = 0; i < 2756; ++i) sf_seq_advance(&seq, 120.0f, 44100.0f, ev, SF_SEQ_EVENTS);
    assert(fabsf(sf_seq_lane_value(&seq, 0) - (-0.125f)) < 0.01f);
    for (int i = 0; i < 5512 * 2; ++i) sf_seq_advance(&seq, 120.0f, 44100.0f, ev, SF_SEQ_EVENTS);
    assert(fabsf(sf_seq_lane_value(&seq, 0) - 0.25f) < 0.01f);

    /* The offset is additive on the destination, and macros can be pushed. */
    {
        int dest[SF_MOD_COLUMNS] = {0};
        float amount[SF_MOD_COLUMNS * MODSRC_COUNT] = {0};
        int macro_dest[SF_MACROS * SF_MACRO_SLOTS] = {0};
        float macro_amount[SF_MACROS * SF_MACRO_SLOTS] = {0};
        float source[MODSRC_COUNT] = {0};
        float macros[SF_MACROS] = {0};
        int lane_dest[2] = {DEST_CUTOFF, DEST_MACRO1};
        float lane_value[2] = {-0.25f, 0.5f};
        dest[0] = DEST_CUTOFF;
        amount[MODSRC_LFO1] = 0.5f;
        source[MODSRC_LFO1] = 1.0f;
        macro_dest[0] = DEST_RESONANCE;
        macro_amount[0] = 0.4f;
        sf_mod_input_t in = { dest, amount, macro_dest, macro_amount, source, macros,
                              lane_dest, lane_value, 2 };
        sf_mod_values_t out;
        sf_mod_apply(&in, &out);
        assert(fabsf(out.dest[DEST_CUTOFF] - 0.25f) < 1e-6f);
        assert(fabsf(out.dest[DEST_RESONANCE] - 0.2f) < 1e-6f);
    }

    /* Through the plugin: a stepped cutoff lane closes the filter on every
     * second step, so those steps come out far quieter. */
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    void *inst = api->create_instance(".", "{}");
    assert(inst);
    api->set_param(inst, "seq_data", "60.100.0,60.100.0,60.100.0,60.100.0");
    api->set_param(inst, "seq_length", "4");
    api->set_param(inst, "seq_gate", "1");
    api->set_param(inst, "seq_lane1_dest", "2");
    api->set_param(inst, "seq_lane1_smooth", "0");
    api->set_param(inst, "seq_lane1_data", "0,-0.8,0,-0.8");
    api->set_param(inst, "seq_mode", "2");
    char value[64];
    read_value(api, inst, "seq_lane1_dest", value, sizeof(value));
    assert(strcmp(value, "2") == 0);
    read_value(api, inst, "seq_lane1_smooth", value, sizeof(value));
    assert(strcmp(value, "0") == 0);
    read_value(api, inst, "seq_lane1_data", text, sizeof(text));
    assert(strcmp(text, ",-0.8,,-0.8") == 0);
    uint8_t note_on[3] = {0x90, 60, 100}, note_off[3] = {0x80, 60, 0};
    api->on_midi(inst, note_on, 3, 0);
    int16_t audio[128 * 2];
    double step_energy[8] = {0};
    long frame = 0;
    for (int b = 0; b < 345; ++b) { /* two loops of four 1/16 steps */
        api->render_block(inst, audio, 128);
        for (int i = 0; i < 128; ++i, ++frame) {
            int step = (int)((double)frame / 5512.5);
            if (step < 8) step_energy[step] += fabs((double)audio[2 * i]);
        }
    }
    api->on_midi(inst, note_off, 3, 0);
    double open = step_energy[4] + step_energy[6];
    double closed = step_energy[5] + step_energy[7];
    /* About 0.55: part of each closed step is the cutoff still gliding down,
     * which the filter's (measured, small-signal linear) saturation no longer
     * squashes. */
    assert(open > 0.0 && closed < 0.6 * open);

    /* State round trip keeps the lane; a lane-free state clears it. */
    static char state[65536];
    assert(api->get_param(inst, "state", state, sizeof(state)) > 0);
    assert(strstr(state, "\"seq_lane1_dest\":2,\"seq_lane1_smooth\":0,"
                         "\"seq_lane1_data\":\",-0.8,,-0.8\"") != NULL);
    assert(strstr(state, "seq_lane2") == NULL);
    void *other = api->create_instance(".", "{}");
    api->set_param(other, "state", state);
    read_value(api, other, "seq_lane1_data", text, sizeof(text));
    assert(strcmp(text, ",-0.8,,-0.8") == 0);
    read_value(api, other, "seq_lane1_dest", value, sizeof(value));
    assert(strcmp(value, "2") == 0);
    api->set_param(other, "state", "{\"version\":13,\"seq_mode\":2,\"seq_data\":\"60.100.0\"}");
    read_value(api, other, "seq_lane1_dest", value, sizeof(value));
    assert(strcmp(value, "0") == 0);
    read_value(api, other, "seq_lane1_smooth", value, sizeof(value));
    assert(strcmp(value, "1") == 0);
    api->destroy_instance(other);

    /* The factory bank carries the MiniFreak's lanes: Enou automates Release
     * and Cutoff. */
    read_value(api, inst, "preset_count", value, sizeof(value));
    int count = atoi(value);
    int found = 0;
    for (int i = 0; i < count && !found; ++i) {
        char index_text[16];
        snprintf(index_text, sizeof(index_text), "%d", i);
        api->set_param(inst, "preset", index_text);
        read_value(api, inst, "preset_name", value, sizeof(value));
        if (strcmp(value, "Enou") != 0) continue;
        found = 1;
        read_value(api, inst, "seq_lane1_dest", value, sizeof(value));
        assert(strcmp(value, "21") == 0); /* Release */
        read_value(api, inst, "seq_lane2_dest", value, sizeof(value));
        assert(strcmp(value, "2") == 0);  /* Cutoff */
        read_value(api, inst, "seq_lane2_data", text, sizeof(text));
        assert(text[0] == '-');
    }
    assert(found || count <= SF_ORIGINAL_PRESET_COUNT); /* the bank is optional at build time */
    api->destroy_instance(inst);
}

/* Macros are modulation destinations: a grid column can sweep a macro and the
 * macro's own sockets follow it. */
static void test_macro_destination(void) {
    int dest[SF_MOD_COLUMNS] = {0};
    float amount[SF_MOD_COLUMNS * MODSRC_COUNT] = {0};
    int macro_dest[SF_MACROS * SF_MACRO_SLOTS] = {0};
    float macro_amount[SF_MACROS * SF_MACRO_SLOTS] = {0};
    float source[MODSRC_COUNT] = {0};
    float macros[SF_MACROS] = {0.25f, 0.0f, 0.0f, 0.0f};
    dest[0] = DEST_MACRO1;
    amount[0 * MODSRC_COUNT + MODSRC_LFO1] = 0.5f;
    macro_dest[0] = DEST_CUTOFF;          /* macro 1, socket 1 */
    macro_amount[0] = 0.8f;
    macro_dest[1] = DEST_MACRO2;          /* a socket cannot target a macro */
    macro_amount[1] = 1.0f;
    source[MODSRC_LFO1] = 1.0f;
    sf_mod_input_t in = { dest, amount, macro_dest, macro_amount, source, macros, NULL, NULL, 0 };
    sf_mod_values_t out;
    sf_mod_apply(&in, &out);
    /* macro 1 = 0.25 + 0.5 = 0.75; cutoff gets 0.75 * 0.8 = 0.6 */
    assert(fabsf(out.dest[DEST_CUTOFF] - 0.6f) < 1e-5f);
    assert(out.dest[DEST_MACRO2] == 0.0f);
    /* The sum clamps to the macro's 0..1 range. */
    source[MODSRC_LFO1] = 4.0f;
    sf_mod_apply(&in, &out);
    assert(fabsf(out.dest[DEST_CUTOFF] - 0.8f) < 1e-5f);
    source[MODSRC_LFO1] = -4.0f;
    sf_mod_apply(&in, &out);
    assert(fabsf(out.dest[DEST_CUTOFF]) < 1e-5f);
}

/* Matrix cells are destinations, as on the MiniFreak ("Mod 4:3" is LFO 2 into
 * column 3): a column's Envelope can set how much LFO 2 reaches Osc 1 Timbre,
 * and a macro slot can aim at the same cell. */
static void test_cell_destinations(void) {
    int dest[SF_MOD_COLUMNS] = {0};
    float amount[SF_MOD_COLUMNS * MODSRC_COUNT] = {0};
    int macro_dest[SF_MACROS * SF_MACRO_SLOTS] = {0};
    float macro_amount[SF_MACROS * SF_MACRO_SLOTS] = {0};
    float source[MODSRC_COUNT] = {0};
    float macros[SF_MACROS] = {0.0f, 0.0f, 0.5f, 0.0f};
    int cell_4_3 = DEST_CELL_BASE + MODSRC_LFO2 * SF_MOD_COLUMNS + 2;
    dest[2] = DEST_SHAPE1;                         /* column 3 */
    amount[2 * MODSRC_COUNT + MODSRC_LFO2] = 0.1f;
    dest[6] = cell_4_3;                            /* column 7: Mod 4:3 */
    amount[6 * MODSRC_COUNT + MODSRC_ENV] = 0.5f;
    source[MODSRC_LFO2] = 1.0f;
    source[MODSRC_ENV] = 1.0f;
    sf_mod_input_t in = { dest, amount, macro_dest, macro_amount, source, macros, NULL, NULL, 0 };
    sf_mod_values_t out;
    sf_mod_apply(&in, &out);
    /* depth 0.1 + 0.5 * Envelope = 0.6, times LFO 2 */
    assert(fabsf(out.dest[DEST_SHAPE1] - 0.6f) < 1e-5f);
    /* A macro slot aimed at the cell adds macro * amount; the depth clamps. */
    dest[6] = DEST_OFF;
    macro_dest[2 * SF_MACRO_SLOTS] = cell_4_3;     /* macro 3, socket 1 */
    macro_amount[2 * SF_MACRO_SLOTS] = 0.8f;
    sf_mod_apply(&in, &out);
    assert(fabsf(out.dest[DEST_SHAPE1] - 0.5f) < 1e-5f);   /* 0.1 + 0.4 */
    macros[2] = 1.0f;
    macro_amount[2 * SF_MACRO_SLOTS] = 3.0f;
    sf_mod_apply(&in, &out);
    assert(fabsf(out.dest[DEST_SHAPE1] - 1.0f) < 1e-5f);
    /* A cell in another column or row leaves this one alone. */
    macro_dest[2 * SF_MACRO_SLOTS] = DEST_CELL_BASE + MODSRC_LFO1 * SF_MOD_COLUMNS + 2;
    sf_mod_apply(&in, &out);
    assert(fabsf(out.dest[DEST_SHAPE1] - 0.1f) < 1e-5f);

    /* Through the plugin: the names, the keys, and the device list. */
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    void *inst = api->create_instance("", "{}");
    char value[64];
    api->set_param(inst, "modcol7_dest", "M4:3");
    api->set_param(inst, "macro3_dest1", "M4:3");
    read_value(api, inst, "modcol7_dest", value, sizeof(value));
    assert(strcmp(value, "M4:3") == 0);
    read_value(api, inst, "macro3_dest1", value, sizeof(value));
    assert(strcmp(value, "M4:3") == 0);
    static char state[65536];
    read_value(api, inst, "state", state, sizeof(state));
    void *other = api->create_instance("", "{}");
    api->set_param(other, "state", state);
    read_value(api, other, "macro3_dest1", value, sizeof(value));
    assert(strcmp(value, "M4:3") == 0);
    read_value(api, inst, "chain_params", state, sizeof(state));
    assert(strstr(state, "\"M7:12\",\"FFM\"]") != NULL);
    api->destroy_instance(other);
    api->destroy_instance(inst);
}

/* Filter FM: off at 0 (identical audio), audible at depth, and each source
 * (Osc 1, Osc 2, Mix) gives a different result. */
static void fm_render(plugin_api_v2_t *api, int16_t *out, const char *defaults,
                      const char *const (*set)[2], int n) {
    void *inst = api->create_instance("", defaults);
    assert(inst);
    for (int k = 0; k < n; ++k) api->set_param(inst, set[k][0], set[k][1]);
    int16_t warm[256];
    for (int b = 0; b < 400; ++b) api->render_block(inst, warm, 128);
    uint8_t note_on[3] = {0x90, 48, 120};
    api->on_midi(inst, note_on, 3, 0);
    for (int b = 0; b < 8; ++b) api->render_block(inst, out + b * 256, 128);
    api->destroy_instance(inst);
}

static int fm_diff(const int16_t *a, const int16_t *b) {
    int d = 0;
    for (int i = 0; i < 2048; ++i) d += a[i] != b[i];
    return d;
}

/* Filter FM: off at 0 (identical audio), audible at depth, each source (Osc 1,
 * Osc 2, Mix) gives a different result and is taken before its level knob, the
 * FFM matrix destination matches the knob, and the setting survives a state
 * round trip. */
static void test_filter_fm(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    const char *defaults = "{\"attack\":1,\"sustain\":1,\"output\":1,"
                           "\"osc1_level\":0.8,\"osc2_level\":0.8,\"osc2_tune\":7,\"stereo\":0,"
                           "\"cutoff\":0.5,\"filter_env\":0}";
    static int16_t base[2048], on[2048], o1[2048], mix[2048], neg[2048], other[2048];
    typedef const char *const pair[2];
    fm_render(api, base, defaults, (pair[]){{"filter_fm", "0"}}, 1);
    fm_render(api, on, defaults, (pair[]){{"filter_fm", "1"}, {"filter_fm_src", "OSC 2"}}, 2);
    fm_render(api, o1, defaults, (pair[]){{"filter_fm", "1"}, {"filter_fm_src", "OSC 1"}}, 2);
    fm_render(api, mix, defaults, (pair[]){{"filter_fm", "1"}, {"filter_fm_src", "Mix"}}, 2);
    fm_render(api, neg, defaults, (pair[]){{"filter_fm", "-1"}, {"filter_fm_src", "OSC 2"}}, 2);
    assert(fm_diff(base, on) > 100);
    assert(fm_diff(on, o1) > 100 && fm_diff(on, mix) > 100 && fm_diff(o1, mix) > 100);
    assert(fm_diff(on, neg) > 100);
    fm_render(api, other, defaults, (pair[]){{"filter_fm", "0"}, {"filter_fm_src", "OSC 1"}}, 2);
    assert(fm_diff(base, other) == 0);

    /* The source is taken before the level knob: a muted Osc 2 still
     * modulates, and a silent Osc 1 cannot. */
    fm_render(api, base, defaults, (pair[]){{"osc2_level", "0"}, {"filter_fm", "0"}}, 2);
    fm_render(api, on, defaults, (pair[]){{"osc2_level", "0"}, {"filter_fm", "1"}, {"filter_fm_src", "OSC 2"}}, 3);
    assert(fm_diff(base, on) > 100);

    /* A processor engine in Osc 2 modulates with its wet signal. */
    fm_render(api, base, defaults, (pair[]){{"osc2_mode", "Crusher"}, {"filter_fm", "0"}}, 2);
    fm_render(api, on, defaults, (pair[]){{"osc2_mode", "Crusher"}, {"filter_fm", "1"}}, 2);
    assert(fm_diff(base, on) > 100);

    /* FFM: a macro aimed at the destination does what the knob does. */
    fm_render(api, o1, defaults, (pair[]){{"filter_fm", "0.5"}}, 1);
    fm_render(api, mix, defaults, (pair[]){{"macro1_dest1", "FFM"}, {"macro1_amt1", "0.5"}, {"macro1", "1"}}, 3);
    {
        int worst = 0;
        for (int i = 0; i < 2048; ++i) worst = abs(o1[i] - mix[i]) > worst ? abs(o1[i] - mix[i]) : worst;
        assert(worst <= 8); /* the knob is smoothed, the route is not */
    }
    fm_render(api, base, defaults, (pair[]){{"filter_fm", "0"}}, 1);
    assert(fm_diff(base, mix) > 100);

    /* Every Mod Rate setting renders finite, audible output with FM on. */
    const char *rates[4] = {"Full", "1/2", "1/4", "1/8"};
    for (int r = 0; r < 4; ++r) {
        fm_render(api, base, defaults, (pair[]){{"control_rate", rates[r]}, {"filter_fm", "0"}}, 2);
        fm_render(api, on, defaults, (pair[]){{"control_rate", rates[r]}, {"filter_fm", "1"}}, 2);
        assert(fm_diff(base, on) > 100);
    }

    /* Self-oscillating resonance with full FM either way stays sound and bounded. */
    const char *sign[2] = {"1", "-1"};
    for (int k = 0; k < 2; ++k) {
        fm_render(api, on, defaults, (pair[]){{"resonance", "1"}, {"cutoff", "0.8"}, {"filter_fm", sign[k]}}, 3);
        int loud = 0, maxv = 0;
        for (int i = 0; i < 2048; ++i) {
            if (on[i]) loud = 1;
            if (abs(on[i]) > maxv) maxv = abs(on[i]);
        }
        assert(loud && maxv < 32000);
    }

    /* State round trip, and a state from before Filter FM resets it. */
    void *inst = api->create_instance("", defaults);
    api->set_param(inst, "filter_fm", "0.4");
    api->set_param(inst, "filter_fm_src", "Mix");
    static char state[65536];
    read_value(api, inst, "state", state, sizeof(state));
    void *copy = api->create_instance("", defaults);
    api->set_param(copy, "state", state);
    char value[64];
    read_value(api, copy, "filter_fm_src", value, sizeof(value));
    assert(strcmp(value, "Mix") == 0);
    read_value(api, copy, "filter_fm", value, sizeof(value));
    assert(fabs(atof(value) - 0.4) < 1e-5);
    char *key = strstr(state, ",\"filter_fm\":");
    assert(key);
    char *src_key = strstr(state, ",\"filter_fm_src\":");
    assert(src_key);
    src_key[2] = 'x'; /* rename so the loader no longer sees them */
    key[2] = 'x';
    api->set_param(copy, "state", state);
    read_value(api, copy, "filter_fm", value, sizeof(value));
    assert(fabs(atof(value)) < 1e-9);
    read_value(api, copy, "filter_fm_src", value, sizeof(value));
    assert(strcmp(value, "OSC 2") == 0);
    api->destroy_instance(copy);
    api->destroy_instance(inst);

    /* A non-finite value does not turn into full negative FM. */
    inst = api->create_instance("", defaults);
    api->set_param(inst, "filter_fm", "nan");
    read_value(api, inst, "filter_fm", value, sizeof(value));
    assert(fabs(atof(value)) < 1e-9);
    api->destroy_instance(inst);
}

/* Keyboard Chord and the Unison modes, measured on MiniFreak V: a chord plays
 * every note in Poly and Mono, takes all six voices in Unison whatever the
 * Count, and the key's note-off releases the whole chord. */
static int active_voices(plugin_api_v2_t *api, void *instance) {
    char value[32];
    read_value(api, instance, "polyphony", value, sizeof(value));
    return atoi(value);
}

static void test_keyboard_chord_and_unison_modes(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    void *instance = api->create_instance("", "{\"attack\":1,\"release\":1,\"output\":1}");
    assert(instance != NULL);
    int16_t block[256];
    uint8_t on[3] = {0x90, 60, 100};
    uint8_t off[3] = {0x80, 60, 0};
    char value[65536];

    api->set_param(instance, "chord", "On");
    api->set_param(instance, "chord_notes", "2");
    api->set_param(instance, "chord_note1", "4");
    api->set_param(instance, "chord_note2", "7");
    const char *modes[] = {"Poly", "Mono", "Unison"};
    const int expect[] = {3, 3, 6};
    for (int m = 0; m < 3; ++m) {
        api->set_param(instance, "voice_mode", modes[m]);
        api->set_param(instance, "unison_voices", "2");
        api->on_midi(instance, on, 3, 0);
        api->render_block(instance, block, 128);
        if (active_voices(api, instance) != expect[m])
            fprintf(stderr, "%s chord: %d voices\n", modes[m], active_voices(api, instance));
        assert(active_voices(api, instance) == expect[m]);
        api->on_midi(instance, off, 3, 0);
        for (int i = 0; i < 400; ++i) api->render_block(instance, block, 128);
        assert(active_voices(api, instance) == 0);
    }

    read_value(api, instance, "state", value, sizeof(value));
    assert(strstr(value, "\"chord\":1,\"chord_notes\":2,\"chord_note1\":4,\"chord_note2\":7") != NULL);
    read_value(api, instance, "chord", value, sizeof(value));
    assert(strcmp(value, "On") == 0);

    /* Poly Unison: each key its own stack, two held keys sharing the six. */
    api->set_param(instance, "chord", "Off");
    api->set_param(instance, "unison_mode", "Poly");
    api->set_param(instance, "unison_voices", "6");
    uint8_t on2[3] = {0x90, 67, 100};
    uint8_t off2[3] = {0x80, 67, 0};
    api->on_midi(instance, on, 3, 0);
    api->render_block(instance, block, 128);
    assert(active_voices(api, instance) == 6);
    api->on_midi(instance, on2, 3, 0);
    for (int i = 0; i < 4; ++i) api->render_block(instance, block, 128);
    assert(active_voices(api, instance) == 6);
    api->on_midi(instance, off, 3, 0);
    api->on_midi(instance, off2, 3, 0);
    for (int i = 0; i < 400; ++i) api->render_block(instance, block, 128);
    assert(active_voices(api, instance) == 0);

    /* PS1 imports its chord (+12, +19 over the key) and a fifth-wide step. */
    read_value(api, instance, "preset_names", value, sizeof(value));
    const char *cursor = value;
    int index = 0, ps1 = -1;
    while ((cursor = strchr(cursor, '"')) != NULL) {
        const char *end = strchr(++cursor, '"');
        if (!end) break;
        if (end - cursor == 3 && memcmp(cursor, "PS1", 3) == 0) { ps1 = index; break; }
        ++index;
        cursor = end + 1;
    }
    if (ps1 >= 0) {
        char text[16];
        snprintf(text, sizeof(text), "%d", ps1);
        api->set_param(instance, "preset", text);
        read_value(api, instance, "chord", value, sizeof(value));
        assert(strcmp(value, "On") == 0);
        read_value(api, instance, "chord_notes", value, sizeof(value));
        assert(atoi(value) == 2);
        read_value(api, instance, "chord_note1", value, sizeof(value));
        assert(atoi(value) == 12);
        read_value(api, instance, "chord_note2", value, sizeof(value));
        assert(atoi(value) == 19);
        read_value(api, instance, "spread", value, sizeof(value));
        assert(fabsf(strtof(value, NULL) / 6.0f - 701.0f) < 5.0f);
        read_value(api, instance, "unison_mode", value, sizeof(value));
        assert(strcmp(value, "Mono") == 0);
    }
    api->destroy_instance(instance);
}

/* Move's knob grid has eight cells: a level with more knobs spills into a
 * "<Level> - 2" page. Only the effect pages list more, and their visible_if
 * gates leave at most eight for any one effect type. */
static void test_device_page_layout(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    void *instance = api->create_instance("", NULL);
    assert(instance);
    char value[65536];
    read_value(api, instance, "ui_hierarchy", value, sizeof(value));

    assert(strstr(value, "\"root\":{\"label\":\"Bizar\",\"knobs\":[\"macro1\",\"macro2\",\"macro3\","
                         "\"output\",\"cutoff\",\"resonance\",\"attack\",\"release\"]") != NULL);
    assert(strstr(value, "\"knobs\":[\"osc1_mode\",\"osc1_wave\",\"osc1_timbre\",\"osc1_shape\"") != NULL);
    assert(strstr(value, "\"knobs\":[\"lfo1_shape\",\"lfo1_rate\",\"lfo1_depth\"") != NULL);
    assert(strstr(value, "\"settings\":{\"label\":\"Settings\",\"knobs\":[\"osc_phase\",\"control_rate\"]") != NULL);
    assert(strstr(value, "cycle_legacy") == NULL);
    /* Each LFO's curve page follows it in the jog order. */
    assert(strstr(value, "{\"level\":\"lfo1\",\"label\":\"LFO 1\"},{\"level\":\"lfo1_curve\"") != NULL);
    assert(strstr(value, "{\"level\":\"lfo2\",\"label\":\"LFO 2\"},{\"level\":\"lfo2_curve\"") != NULL);

    static const char tag[] = "\"knobs\":[";
    for (const char *at = strstr(value, tag); at; at = strstr(at + 1, tag)) {
        const char *list = at + sizeof(tag) - 1;
        const char *end = strchr(list, ']');
        assert(end);
        int count = list == end ? 0 : 1;
        for (const char *c = list; c < end; ++c) if (*c == ',') ++count;
        int gated_fx = strncmp(list, "\"fx", 3) == 0;
        assert(count <= 8 || gated_fx);
    }
    api->destroy_instance(instance);
}

/* Mod Rate is a CPU setting: it round-trips through state, survives a preset
 * load, and changes how often modulation updates without changing the sound
 * beyond a trace. */
static void test_mod_rate_setting(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    const char *defaults = "{\"attack\":1,\"decay\":100,\"sustain\":1,\"output\":1,"
                           "\"osc1_level\":1,\"osc2_level\":0,\"stereo\":0,"
                           "\"lfo1_rate\":40,\"lfo1_shape\":\"Triangle\"}";
    void *full = api->create_instance("", defaults);
    void *eighth = api->create_instance("", defaults);
    assert(full && eighth);
    char value[16384];

    read_value(api, full, "control_rate", value, sizeof(value));
    assert(strcmp(value, "1/2") == 0);
    api->set_param(full, "control_rate", "Full");
    api->set_param(eighth, "control_rate", "1/8");
    read_value(api, eighth, "control_rate", value, sizeof(value));
    assert(strcmp(value, "1/8") == 0);

    read_value(api, eighth, "state", value, sizeof(value));
    assert(strstr(value, "\"control_rate\":3") != NULL);
    void *restored = api->create_instance("", NULL);
    assert(restored);
    api->set_param(restored, "state", value);
    read_value(api, restored, "control_rate", value, sizeof(value));
    assert(strcmp(value, "1/8") == 0);
    api->set_param(restored, "preset", "1");
    read_value(api, restored, "control_rate", value, sizeof(value));
    assert(strcmp(value, "1/8") == 0);
    api->destroy_instance(restored);

    for (int k = 0; k < 2; ++k) {
        void *inst = k ? eighth : full;
        api->set_param(inst, "modcol1_dest", "Ptc1");
        api->set_param(inst, "modcol1_lfo1", "0.05");
        api->set_param(inst, "modcol2_dest", "Cut");
        api->set_param(inst, "modcol2_lfo1", "0.5");
    }
    uint8_t note_on[3] = {0x90, 60, 110};
    api->on_midi(full, note_on, 3, 0);
    api->on_midi(eighth, note_on, 3, 0);
    int16_t a[256], b[256];
    double ea = 0.0, eb = 0.0;
    int differs = 0;
    for (int blk = 0; blk < 40; ++blk) {
        api->render_block(full, a, 128);
        api->render_block(eighth, b, 128);
        for (int i = 0; i < 256; ++i) {
            ea += (double)a[i] * a[i];
            eb += (double)b[i] * b[i];
            if (a[i] != b[i]) differs++;
        }
    }
    assert(ea > 0.0 && differs > 0);
    assert(eb > ea * 0.9 && eb < ea * 1.1);
    api->destroy_instance(full);
    api->destroy_instance(eighth);
}

static int polyphony(plugin_api_v2_t *api, void *instance) {
    char value[32];
    read_value(api, instance, "polyphony", value, sizeof(value));
    return atoi(value);
}

static void midi3(plugin_api_v2_t *api, void *instance, int a, int b, int c) {
    uint8_t m[3] = {(uint8_t)a, (uint8_t)b, (uint8_t)c};
    api->on_midi(instance, m, 3, 0);
}

static void render_blocks(plugin_api_v2_t *api, void *instance, int blocks) {
    int16_t block[256];
    for (int i = 0; i < blocks; ++i) api->render_block(instance, block, 128);
}

/* CC 64 holds released keys until the pedal comes up, in Poly, Mono and the
 * sequencer alike; CC 121 lets go of the pedal. */
static void test_sustain_pedal(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    void *instance = api->create_instance("", "{\"attack\":1,\"decay\":20,\"sustain\":1,\"release\":1,\"output\":1}");
    assert(instance != NULL);

    midi3(api, instance, 0xb0, 64, 127);
    midi3(api, instance, 0x90, 60, 100);
    midi3(api, instance, 0x90, 64, 100);
    midi3(api, instance, 0x80, 60, 0);
    midi3(api, instance, 0x80, 64, 0);
    render_blocks(api, instance, 40);
    assert(polyphony(api, instance) == 2);
    midi3(api, instance, 0xb0, 64, 0);
    render_blocks(api, instance, 40);
    assert(polyphony(api, instance) == 0);

    /* A key released while its voice is still waiting on a steal fade. */
    midi3(api, instance, 0xb0, 64, 127);
    for (int n = 50; n < 56; ++n) midi3(api, instance, 0x90, n, 100);
    render_blocks(api, instance, 4);
    midi3(api, instance, 0x90, 70, 100);
    midi3(api, instance, 0x80, 70, 0);
    render_blocks(api, instance, 40);
    char notes[256];
    read_value(api, instance, "sounding_notes", notes, sizeof(notes));
    assert(strstr(notes, "70") != NULL);
    for (int n = 50; n < 56; ++n) midi3(api, instance, 0x80, n, 0);
    midi3(api, instance, 0xb0, 121, 0);   /* Reset All Controllers releases the pedal */
    render_blocks(api, instance, 40);
    assert(polyphony(api, instance) == 0);

    /* Mono: the last key released with the pedal down keeps sounding. */
    api->set_param(instance, "voice_mode", "Mono");
    midi3(api, instance, 0xb0, 64, 127);
    midi3(api, instance, 0x90, 60, 100);
    midi3(api, instance, 0x80, 60, 0);
    render_blocks(api, instance, 40);
    assert(polyphony(api, instance) == 1);
    midi3(api, instance, 0xb0, 64, 0);
    render_blocks(api, instance, 40);
    assert(polyphony(api, instance) == 0);

    /* Arp: a key released with the pedal down stays in the arpeggio. */
    api->set_param(instance, "voice_mode", "Poly");
    api->set_param(instance, "seq_mode", "Arp");
    midi3(api, instance, 0xb0, 64, 127);
    midi3(api, instance, 0x90, 60, 100);
    midi3(api, instance, 0x80, 60, 0);
    int played = 0;
    for (int b = 0; b < 400; ++b) {
        render_blocks(api, instance, 1);
        played |= polyphony(api, instance) > 0;
    }
    assert(played);
    midi3(api, instance, 0xb0, 64, 0);
    render_blocks(api, instance, 400);
    assert(polyphony(api, instance) == 0);
    api->destroy_instance(instance);
}

static float g_test_beat;
static float test_bpm(void) { return 120.0f; }
static int test_clock(void) { return MOVE_CLOCK_STATUS_RUNNING; }
static double test_beat(void) { return (double)g_test_beat; }

/* With the host transport running, a synced free LFO takes its phase from the
 * beat: a falling Saw at 1/4 reads 1 - 2 * frac(beat). */
static void test_host_transport_lfo(void) {
    host_api_v1_t host;
    memset(&host, 0, sizeof(host));
    host.get_bpm = test_bpm;
    host.get_clock_status = test_clock;
    host.get_beat_position = test_beat;
    plugin_api_v2_t *api = move_plugin_init_v2(&host);
    void *instance = api->create_instance("", NULL);
    assert(instance != NULL);
    api->set_param(instance, "lfo1_shape", "Saw");
    api->set_param(instance, "lfo1_sync", "On");
    api->set_param(instance, "lfo1_div", "1/4");
    api->set_param(instance, "lfo1_retrig", "Free");
    midi3(api, instance, 0x90, 60, 100);
    const float beats[2] = {10.25f, 31.75f};
    for (int k = 0; k < 2; ++k) {
        g_test_beat = beats[k];
        render_blocks(api, instance, 1);
        char text[256];
        read_value(api, instance, "mod_sources", text, sizeof(text));
        float v[7];
        assert(sscanf(text, "[%f,%f,%f,%f,%f,%f,%f]", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) == 7);
        float want = 1.0f - 2.0f * (beats[k] - floorf(beats[k]));
        if (fabsf(v[2] - want) > 0.01f)
            fprintf(stderr, "beat %g: lfo1 %g, want %g\n", (double)beats[k], (double)v[2], (double)want);
        assert(fabsf(v[2] - want) < 0.01f);
    }
    api->destroy_instance(instance);
    move_plugin_init_v2(NULL);
}

/* Fills the stack below the caller with NaN, so a local the render block
 * reads before writing shows up as NaN rather than as a plausible leftover. */
__attribute__((noinline)) static float poison_stack(void) {
    volatile float junk[16384];
    for (int i = 0; i < 16384; ++i) junk[i] = NAN;
    return junk[0];
}

__attribute__((noinline)) static void render_poisoned(plugin_api_v2_t *api, void *instance) {
    int16_t block[256];
    (void)poison_stack();
    api->render_block(instance, block, 128);
}

/* The arpeggiator starts notes mid-block on voices that were idle when the
 * block began. Their Cycling Envelope steps were left unset, and a Run-mode
 * envelope never resets, so a NaN there stayed in the voice for good. */
static void test_seq_note_gets_cycle_steps(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    void *instance = api->create_instance("", NULL);
    assert(instance != NULL);
    api->set_param(instance, "cycle_mode", "Run");
    api->set_param(instance, "seq_mode", "1");
    api->set_param(instance, "seq_div", "13");
    midi3(api, instance, 0x90, 60, 100);
    char text[256];
    for (int i = 0; i < 400; ++i) {
        render_poisoned(api, instance);
        read_value(api, instance, "mod_sources", text, sizeof(text));
        assert(strstr(text, "nan") == NULL && strstr(text, "inf") == NULL);
    }
    api->destroy_instance(instance);
}

/* A state blob leaves out a default Shaper extension and an empty sequencer,
 * so recalling one must reset them, not keep the previous patch's. */
static void test_state_recall_resets_omitted_sections(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    void *init = api->create_instance("", NULL);
    static char blob[65536];
    read_value(api, init, "state", blob, sizeof(blob));
    assert(strstr(blob, "lfo1_shp_mode") == NULL && strstr(blob, "seq_mode") == NULL);
    api->destroy_instance(init);

    void *instance = api->create_instance("", NULL);
    api->set_param(instance, "lfo1_shp_mode", "1");
    api->set_param(instance, "lfo1_shp_steps", "5");
    api->set_param(instance, "lfo2_shp_curve12", "0.9");
    api->set_param(instance, "seq_mode", "1");
    api->set_param(instance, "seq_data", "60.100.0,64.100.0");
    api->set_param(instance, "state", blob);
    char value[64];
    read_value(api, instance, "lfo1_shp_mode", value, sizeof(value));
    assert(strcmp(value, "0") == 0);
    read_value(api, instance, "lfo1_shp_steps", value, sizeof(value));
    assert(strcmp(value, "8") == 0);
    read_value(api, instance, "lfo2_shp_curve12", value, sizeof(value));
    assert(strcmp(value, "0.5") == 0);
    read_value(api, instance, "seq_mode", value, sizeof(value));
    assert(strcmp(value, "0") == 0);
    read_value(api, instance, "seq_data", value, sizeof(value));
    assert(strchr(value, '6') == NULL);
    api->destroy_instance(instance);
}

/* Switching a slot from Reverb to Delay used to read the reverb's lines (the
 * start of the shared ring) back as echoes. With nothing playing, the Delay
 * must stay silent. The switch lands where the 1 s read sweeps that region. */
static void test_reverb_to_delay_switch_is_clean(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    void *instance = api->create_instance("", NULL);
    api->set_param(instance, "fx1_type", "Reverb");
    api->set_param(instance, "fx1_mix", "0.5");
    midi3(api, instance, 0x90, 48, 120);
    render_blocks(api, instance, 20);
    midi3(api, instance, 0x80, 48, 0);
    render_blocks(api, instance, 290);
    char value[16];
    read_value(api, instance, "polyphony", value, sizeof(value));
    assert(strcmp(value, "0") == 0);
    api->set_param(instance, "fx1_type", "Delay");
    api->set_param(instance, "fx1_subtype", "0");
    api->set_param(instance, "fx1_time", "1000");
    api->set_param(instance, "fx1_feedback", "0");
    api->set_param(instance, "fx1_mix", "1");
    int16_t block[256];
    long long energy = 0;
    for (int i = 0; i < 360; ++i) {
        api->render_block(instance, block, 128);
        for (int s = 0; s < 256; ++s) energy += (long long)block[s] * block[s];
    }
    assert(energy == 0);
    api->destroy_instance(instance);
}

static sf_voice_start_params_t mono_params(unsigned *age, int env_retrig) {
    sf_voice_start_params_t p = {VOICE_MONO, 0.0f, 0.0f, CYCLE_ENV, 1, 0, 0,
                                 env_retrig, age, SF_MAX_VOICES, 0u, -1};
    return p;
}

/* Mono with Env Reset: a legato note fades the sounding one out (3 ms, as a
 * steal does) instead of cutting it to 0, restarts its envelope from zero,
 * and still glides from the old pitch. Env Continue carries the envelope. */
static void test_mono_env_reset_legato(void) {
    sf_env_shape_t attack, decay;
    float attack_ms = sf_env_shape_setup(&attack, ENV_ATTACK, ENV_CURVE_DEFAULT, 1.0f);
    float decay_ms = sf_env_shape_setup(&decay, ENV_DECAY, ENV_CURVE_DEFAULT, 50.0f);
    float attack_step = 1.0f / (attack_ms * 0.001f * 44100.0f);
    float decay_step = 0.3f / (decay_ms * 0.001f * 44100.0f);
    for (int retrig = 0; retrig < 2; ++retrig) {
        unsigned age = 0;
        sf_voice_t voice;
        memset(&voice, 0, sizeof(voice));
        sf_voice_start_params_t p = mono_params(&age, retrig);
        sf_voice_start(&voice, 0, 48, 100, 0, 0, &p);
        for (int s = 0; s < 8000; ++s)
            sf_voice_envelope_sample(&voice, attack_step, decay_step, 0.7f, &attack, &decay);
        float held = voice.last_env_out;
        float old_hz = voice.current_hz;
        assert(fabsf(held - 0.7f) < 1e-3f);
        sf_voice_start(&voice, 0, 60, 100, 0, 1, &p);
        if (retrig) {
            /* Env Continue: the envelope carries on and the pitch glides. */
            assert(!voice.steal_pending && voice.note == 60);
            assert(voice.current_hz == old_hz && voice.target_hz > 2.0f * old_hz - 1.0f);
            continue;
        }
        assert(voice.steal_pending && voice.note == 48);
        float previous = held;
        int s = 0;
        for (; s < 400 && voice.note != 60; ++s) {
            float out = sf_voice_envelope_sample(&voice, attack_step, decay_step, 0.7f,
                                                 &attack, &decay);
            assert(out <= previous && previous - out < 0.01f);
            previous = out;
        }
        assert(voice.note == 60 && voice.env_stage == ENV_ATTACK && voice.env == 0.0f);
        assert(s > 100 && s < 160);   /* about 3 ms */
        assert(voice.current_hz == old_hz && voice.target_hz > 2.0f * old_hz - 1.0f);
    }

    /* Through the plugin: releasing the new key during that fade, with the
     * old one still held, goes back to the old note instead of leaving the
     * new one sounding; releasing the old key lets the new one start. */
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    for (int release_new = 0; release_new < 2; ++release_new) {
        void *instance = api->create_instance("", NULL);
        api->set_param(instance, "voice_mode", "Mono");
        api->set_param(instance, "env_retrig", "Env Reset");
        midi3(api, instance, 0x90, 48, 100);
        render_blocks(api, instance, 20);
        midi3(api, instance, 0x90, 60, 100);
        midi3(api, instance, 0x80, release_new ? 60 : 48, 0);
        render_blocks(api, instance, 4);
        char value[64];
        read_value(api, instance, "sounding_notes", value, sizeof(value));
        assert(strcmp(value, release_new ? "48" : "60") == 0);
        api->destroy_instance(instance);
    }
}

/* Scenes write an enum as its index; the LFO divisions only took names. */
static void test_lfo_div_accepts_index(void) {
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    void *instance = api->create_instance("", NULL);
    char value[64];
    api->set_param(instance, "lfo1_div", "4");
    read_value(api, instance, "lfo1_div", value, sizeof(value));
    assert(strcmp(value, "4 Bar") == 0);
    api->set_param(instance, "lfo2_div", "13.000000");
    read_value(api, instance, "lfo2_div", value, sizeof(value));
    assert(strcmp(value, "1/2") == 0);
    api->set_param(instance, "lfo2_div", "1/8T");
    read_value(api, instance, "lfo2_div", value, sizeof(value));
    assert(strcmp(value, "1/8T") == 0);
    api->set_param(instance, "lfo2_div", "1/64");   /* no such name: ignored */
    read_value(api, instance, "lfo2_div", value, sizeof(value));
    assert(strcmp(value, "1/8T") == 0);
    api->set_param(instance, "lfo1_div", "99");
    read_value(api, instance, "lfo1_div", value, sizeof(value));
    assert(strcmp(value, "1/32T") == 0);
    api->destroy_instance(instance);
}

/* Wavetable's width warp can round the last phase before 1 up to 1.0, which
 * must read sample 0 again rather than one past the frame (past the whole
 * bank, for the last table). */
static void test_wavetable_phase_wraps(void) {
    for (int table = 0; table < SF_WT_FAMILY_COUNT; ++table) {
        sf_engine_params_t params = {OSC_WAVETABLE, 1.0f, 1.0f, 0.5f, table};
        sf_engine_voice_t start, end;
        memset(&start, 0, sizeof(start));
        start.increment = 0.45f;
        end = start;
        end.phase = nextafterf(1.0f, 0.0f);
        assert(sf_engine1_sample(&params, &end, &end) == sf_engine1_sample(&params, &start, &start));
    }
}

/* A pattern holding more notes than the sequencer tracks (32) must still end
 * every note it starts. */
static void test_seq_note_overflow(void) {
    sf_seq_t seq;
    sf_seq_init(&seq);
    seq.mode = SEQ_MODE_SEQ;
    seq.division = 13;
    char data[SF_SEQ_DATA_MAX];
    int len = 0;
    for (int step = 0; step < 16; ++step) {
        if (step) data[len++] = ',';
        for (int k = 0; k < SF_SEQ_LAYERS; ++k)
            len += snprintf(data + len, sizeof(data) - (size_t)len, "%s%d.100.20",
                            k ? "+" : "", 20 + step * SF_SEQ_LAYERS + k);
    }
    sf_seq_set_data(&seq, data);
    sf_seq_key_down(&seq, 60, 100);
    int sounding[128] = {0};
    int max_events = 0;
    sf_seq_event_t ev[SF_SEQ_EVENTS];
    for (int s = 0; s < 44100 * 4; ++s) {
        int n = sf_seq_advance(&seq, 120.0f, 44100.0f, ev, SF_SEQ_EVENTS);
        if (n > max_events) max_events = n;
        for (int i = 0; i < n; ++i) sounding[ev[i].note] += ev[i].on ? 1 : -1;
    }
    assert(max_events < SF_SEQ_EVENTS);
    int n = sf_seq_panic(&seq, ev, SF_SEQ_EVENTS);
    for (int i = 0; i < n; ++i) sounding[ev[i].note] += ev[i].on ? 1 : -1;
    for (int note = 0; note < 128; ++note) assert(sounding[note] == 0);
}

/* Harmo's cached partial weights must sound exactly like rebuilding them every
 * sample, while Wave and Timbre move and as the pitch changes the count. */
static void test_harmo_weight_cache(void) {
    sf_harmo_cache_t cache;
    memset(&cache, 0, sizeof(cache));
    float stack_a[SF_SUPERWAVE_COPIES] = {0}, stack_b[SF_SUPERWAVE_COPIES] = {0};
    sf_engine_voice_t cached, plain;
    memset(&cached, 0, sizeof(cached));
    memset(&plain, 0, sizeof(plain));
    cached.stack_phase = stack_a;
    cached.harmo_cache = &cache;
    plain.stack_phase = stack_b;
    float phase = 0.0f;
    for (int s = 0; s < 20000; ++s) {
        float increment = s < 10000 ? 0.002f : 0.03f;
        /* Held still for runs of samples, then moved, as a knob would be. */
        float wave = (float)((s / 500) % 7) / 6.0f;
        float timbre = s % 3000 < 1500 ? 0.2f : 0.2f + (float)(s % 1500) / 1500.0f * 0.6f;
        sf_engine_params_t params = {OSC_HARMO, wave, timbre, 0.6f, 0};
        cached.phase = plain.phase = phase;
        cached.increment = plain.increment = increment;
        assert(sf_engine1_sample(&params, &cached, &cached)
               == sf_engine1_sample(&params, &plain, &plain));
        phase += increment;
        if (phase >= 1.0f) phase -= 1.0f;
    }
}

/* One delay-line engine's state, so a cached and an uncached copy can run
 * side by side. */
typedef struct {
    float delay[SF_VOICE_DELAY];
    unsigned pos, excite;
    float low, noise, dc;
    uint32_t rng;
} delay_engine_state_t;

static sf_engine_voice_t delay_engine_voice(delay_engine_state_t *state) {
    sf_engine_voice_t voice;
    memset(&voice, 0, sizeof(voice));
    voice.delay_buf = state->delay;
    voice.delay_pos = &state->pos;
    voice.delay_low = &state->low;
    voice.excitation_left = &state->excite;
    voice.noise_state = &state->noise;
    voice.formant_dc = &state->dc;
    voice.rng = &state->rng;
    voice.gate = 1;
    return voice;
}

/* The Karplus and Comb coefficient caches must sound exactly like working
 * the coefficients out every sample: knobs held, then ramped, the pitch held,
 * then moving every sample (vibrato), through a strike and the bow. */
static void test_delay_engine_caches(void) {
    static delay_engine_state_t cached_state, plain_state;
    for (int engine = 0; engine < 2; ++engine) {
        memset(&cached_state, 0, sizeof(cached_state));
        memset(&plain_state, 0, sizeof(plain_state));
        cached_state.rng = plain_state.rng = 0x12345678u;
        sf_karplus_cache_t karplus;
        sf_comb_cache_t comb;
        memset(&karplus, 0, sizeof(karplus));
        memset(&comb, 0, sizeof(comb));
        sf_engine_voice_t cached = delay_engine_voice(&cached_state);
        sf_engine_voice_t plain = delay_engine_voice(&plain_state);
        cached.karplus_cache = &karplus;
        cached.comb_cache = &comb;
        float phase = 0.0f;
        for (int s = 0; s < 30000; ++s) {
            if (s == 0 || s == 15000) cached_state.excite = plain_state.excite = 300u;
            float increment = 220.0f / 44100.0f;
            if (s >= 10000 && s < 20000) increment *= 1.0f + 0.01f * sinf((float)s * 0.001f);
            float wave = (float)((s / 700) % 5) / 4.0f;
            float timbre = s % 4000 < 2000 ? 0.3f : 0.3f + (float)(s % 2000) / 2000.0f * 0.5f;
            float shape = s < 25000 ? 0.7f : 0.2f;
            cached.phase = plain.phase = phase;
            cached.increment = plain.increment = increment;
            if (engine == 0) {
                sf_engine_params_t params = {OSC_KARPLUS, wave, timbre, shape, 0};
                assert(sf_engine1_sample(&params, &cached, &cached)
                       == sf_engine1_sample(&params, &plain, &plain));
            } else {
                sf_engine_params_t params = {ENGINE_COMB, wave, timbre, shape, 0};
                float input = 0.9f * sf_mod_triangle(phase);
                assert(sf_engine2_mix(&params, &cached, &cached, input, 1.0f, 1.0f, NULL)
                       == sf_engine2_mix(&params, &plain, &plain, input, 1.0f, 1.0f, NULL));
            }
            phase += increment;
            if (phase >= 1.0f) phase -= 1.0f;
        }
        assert(karplus.valid == (engine == 0) && comb.valid == (engine == 1));
    }
}

static int same_bits(float a, float b) {
    return memcmp(&a, &b, sizeof(a)) == 0;
}

/* The tested position's knobs at sample s. Each knob moves on its own while
 * the other two hold, stepped every 250 samples and then ramped every sample
 * (as a knob, an automation lane or an LFO moves them), so a cache that
 * forgets one of its keys is caught. Timbre and Shape move before Wave, whose
 * smoothing in Modal would otherwise rebuild its cache every sample. The
 * pitch moves only while all three hold (cache_test_hz), and the opening
 * hold outlasts Speech's attack click. */
static void cache_test_knobs(int s, float knobs[3]) {
    static const int order[3] = {1, 2, 0};
    knobs[0] = 0.2f;
    knobs[1] = 0.7f;
    knobs[2] = 0.4f;
    if (s >= 2500 && s < 7000) {
        knobs[order[(s - 2500) / 1500]] = (float)(((s - 2500) / 250) % 6) / 5.0f;
    } else if (s >= 7000 && s < 10000) {
        knobs[0] = 0.9f;
        knobs[1] = 0.1f;
        knobs[2] = 0.8f;
    } else if (s >= 10000 && s < 13000) {
        knobs[order[(s - 10000) / 1000]] = (float)(s % 1000) / 1000.0f;
    } else if (s >= 13000 && s < 14000) {
        knobs[0] = 0.3f;
        knobs[1] = 0.6f;
        knobs[2] = 0.2f;
    } else if (s >= 14000) {
        float t = (float)(s % 2000) / 2000.0f;
        knobs[0] = t;
        knobs[1] = 1.0f - t;
        knobs[2] = 0.5f + 0.5f * sinf((float)s * 0.01f);
    }
}

/* Steady, then vibrato and a jump while the knobs hold (7000..10000), then a
 * high note near the engines' pitch clamps. */
static float cache_test_hz(int s) {
    if (s >= 7000 && s < 9000) return 110.0f * (1.0f + 0.02f * sinf((float)s * 0.0007f));
    if (s >= 9000 && s < 10000) return 330.0f;
    if (s >= 15000) return 1500.0f;
    return 110.0f;
}

/* Every engine's coefficient caches must sound exactly like working the
 * coefficients out every sample. Each Osc 1 mode (Osc 2 on BasicWaves) and
 * each Osc 2 mode (Osc 1 on BasicWaves) runs on a voice wired with its caches
 * and on a twin wired without, through each knob stepped and ramped on its
 * own, steady pitch, vibrato and a high note, a closed gate, a new note, a
 * new wavetable and a switch of engine mid-note. */
static void test_engine_caches_match_uncached(void) {
    static sf_voice_t cached, plain;
    for (int position = 1; position <= 2; ++position) {
        int count = position == 1 ? SF_OSC1_MODE_COUNT : SF_OSC2_MODE_COUNT;
        for (int mode = 0; mode < count; ++mode) {
            memset(&cached, 0, sizeof(cached));
            memset(&plain, 0, sizeof(plain));
            unsigned age_cached = 0, age_plain = 0;
            sf_voice_start_params_t pc = mono_params(&age_cached, 1);
            sf_voice_start_params_t pp = mono_params(&age_plain, 1);
            sf_voice_start(&cached, 0, 48, 100, 0, 0, &pc);
            sf_voice_start(&plain, 0, 48, 100, 0, 0, &pp);
            sf_engine_voice_t c1, c2, p1, p2;
            sf_voice_wire_engines(&cached, 1, &c1, &c2);
            sf_voice_wire_engines(&plain, 0, &p1, &p2);
            /* Mid-note, the tested position switches to Wavetable (or, from
             * Wavetable, to Sample, which shares its cache) and back. */
            int partner = position == 1 ? (mode == OSC_WAVETABLE ? OSC_SAMPLE : OSC_WAVETABLE)
                                        : (mode == ENGINE_WAVETABLE ? ENGINE_SAMPLE : ENGINE_WAVETABLE);
            for (int s = 0; s < 16000; ++s) {
                int tested = s >= 13000 && s < 14000 ? partner : mode;
                int mode1 = position == 1 ? tested : OSC_VA;
                int mode2 = position == 2 ? tested : ENGINE_VA;
                if (s == 7000) {
                    /* A new note (the voices are silent: no envelope runs). */
                    sf_voice_start(&cached, 0, 55, 90, 0, 0, &pc);
                    sf_voice_start(&plain, 0, 55, 90, 0, 0, &pp);
                }
                float knobs[3];
                cache_test_knobs(s, knobs);
                float other[3] = {0.5f, 0.35f, 0.5f};
                const float *k1 = position == 1 ? knobs : other;
                const float *k2 = position == 2 ? knobs : other;
                int late = s >= 8200;   /* a new wavetable mid-note */
                sf_engine_params_t prm1 = {mode1, k1[0], k1[1], k1[2], late ? 7 : 3};
                sf_engine_params_t prm2 = {mode2, k2[0], k2[1], k2[2], late ? 5 : 11};
                float inc1 = cache_test_hz(s) / 44100.0f, inc2 = inc1 * 1.5f;
                int gate = s < 8000 || s >= 8500;
                sf_engine_voice_t *pairs[2][2] = {{&c1, &c2}, {&p1, &p2}};
                sf_voice_t *voices[2] = {&cached, &plain};
                float raw1[2], mixed[2], raw2[2];
                for (int v = 0; v < 2; ++v) {
                    sf_voice_t *voice = voices[v];
                    sf_engine_voice_t *o1 = pairs[v][0], *o2 = pairs[v][1];
                    o1->phase = voice->phase1;
                    o1->increment = inc1;
                    o1->sub_phase = voice->sub_phase1;
                    o1->gate = gate;
                    o2->phase = voice->phase2;
                    o2->increment = inc2;
                    o2->sub_phase = voice->sub_phase2;
                    o2->gate = gate;
                    sf_voice_prepare_delay(voice, sf_engine1_uses_delay(mode1),
                                           sf_engine2_uses_delay(mode2));
                    raw1[v] = sf_engine1_sample(&prm1, o1, o2);
                    mixed[v] = sf_engine2_mix(&prm2, o2, o1, raw1[v], 0.8f, 0.6f, &raw2[v]);
                    voice->phase1 += inc1;
                    if (voice->phase1 >= 1.0f) voice->phase1 -= 1.0f;
                    voice->phase2 += inc2;
                    if (voice->phase2 >= 1.0f) voice->phase2 -= 1.0f;
                    voice->sub_phase1 += inc1 * 0.5f;
                    if (voice->sub_phase1 >= 1.0f) voice->sub_phase1 -= 1.0f;
                    voice->sub_phase2 += inc2 * 0.5f;
                    if (voice->sub_phase2 >= 1.0f) voice->sub_phase2 -= 1.0f;
                }
                if (!same_bits(raw1[0], raw1[1]) || !same_bits(mixed[0], mixed[1])
                    || !same_bits(raw2[0], raw2[1])) {
                    fprintf(stderr, "osc%d mode %d differs at sample %d: %.9g/%.9g %.9g/%.9g\n",
                            position, mode, s, (double)raw1[0], (double)raw1[1],
                            (double)mixed[0], (double)mixed[1]);
                    assert(0);
                }
            }
        }
    }
}

/* Osc 2's Wavefolder at full drive reaches about +-19, past the four
 * reflections that cover +-9: it used to clip flat at +-1 for much of each
 * cycle instead of folding. */
static void test_wavefolder_folds_at_full_drive(void) {
    sf_engine_voice_t osc1, osc2;
    memset(&osc1, 0, sizeof(osc1));
    memset(&osc2, 0, sizeof(osc2));
    sf_engine_params_t params = {ENGINE_FOLD, 1.0f, 1.0f, 0.5f, 0};
    int clipped = 0;
    for (int s = 0; s < 4000; ++s) {
        float input = sinf(6.2831853f * (float)s / 400.0f);
        float raw2 = 0.0f;
        (void)sf_engine2_mix(&params, &osc2, &osc1, input, 1.0f, 1.0f, &raw2);
        if (fabsf(raw2) >= 1.0f) ++clipped;
    }
    assert(clipped < 40);   /* under 1%: only the fold's own peaks */
}

/* Speech, sustained: Wave and Shape place the phoneme on a ring of five, so
 * positions 4..5 blend the last phoneme into the first; they used to hold
 * the last one still. Word mode: Timbre's stretch sets the length of the
 * vowel walk as well as of the level (0.85 s at stretch 1, 0.36 s at Timbre
 * 1, where it used to stay 0.85 s). */
static float speech_render(float wave, float timbre, float shape, int samples,
                           sf_speech_cache_t *cache) {
    sf_speech_state_t state;
    memset(&state, 0, sizeof(state));
    sf_engine_voice_t voice;
    memset(&voice, 0, sizeof(voice));
    voice.speech_state = &state;
    voice.speech_cache = cache;
    voice.increment = 130.0f / 44100.0f;
    voice.gate = 1;
    sf_engine_params_t params = {OSC_SPEECH, wave, timbre, shape, 0};
    float sum = 0.0f;
    for (int s = 0; s < samples; ++s) {
        sum += fabsf(sf_engine1_sample(&params, &voice, &voice));
        voice.phase += voice.increment;
        if (voice.phase >= 1.0f) voice.phase -= 1.0f;
    }
    return sum;
}

static void test_speech_phoneme_ring_and_word_length(void) {
    /* Wave 0.25 + Shape 0.8 and 0.9 sit at 4.2 and 4.6. */
    assert(speech_render(0.25f, 0.5f, 0.8f, 8000, NULL)
           != speech_render(0.25f, 0.5f, 0.9f, 8000, NULL));
    sf_speech_cache_t cache;
    memset(&cache, 0, sizeof(cache));
    (void)speech_render(0.6f, 1.0f, 0.5f, 22050, &cache);   /* 0.5 s at Timbre 1 */
    assert(cache.p > 3.99f);
    memset(&cache, 0, sizeof(cache));
    (void)speech_render(0.6f, 0.5f, 0.5f, 22050, &cache);   /* 0.5 s at Timbre 0.5 */
    assert(fabsf(cache.p - 0.5f * 4.0f / 0.85f) < 0.01f);
}

/* Harm's partial 1 plays on the voice phase, ratio 1, but its Nyquist check
 * read 1 + u^3: at Wave 1 a note above about 10.8 kHz went silent. */
static void test_harm_high_note_sounds(void) {
    float stack[SF_SUPERWAVE_COPIES] = {0}, chord[SF_CHORD_PHASES] = {0};
    uint32_t rng = 1u;
    sf_engine_voice_t voice;
    memset(&voice, 0, sizeof(voice));
    voice.stack_phase = stack;
    voice.chord_phase = chord;
    voice.rng = &rng;
    voice.increment = 0.3f;
    sf_engine_params_t params = {OSC_HARM, 1.0f, 0.0f, 0.0f, 0};
    float peak = 0.0f;
    for (int s = 0; s < 1000; ++s) {
        float out = fabsf(sf_engine1_sample(&params, &voice, &voice));
        if (out > peak) peak = out;
        voice.phase += voice.increment;
        if (voice.phase >= 1.0f) voice.phase -= 1.0f;
    }
    assert(peak > 0.05f);
}

/* Audio In's stand-in: the metallic oscillator ran on the voice phase times
 * its ratio, which jumped at every wrap of the voice phase. On its own phase,
 * its half periods are even (to a sample either way). */
static void test_audio_in_metallic_phase_runs_on(void) {
    float noise = 0.0f, stack[SF_SUPERWAVE_COPIES] = {0};
    unsigned count = 0;
    uint32_t rng = 7u;
    sf_engine_voice_t voice;
    memset(&voice, 0, sizeof(voice));
    voice.noise_state = &noise;
    voice.noise_count = &count;
    voice.rng = &rng;
    voice.stack_phase = stack;
    voice.increment = 220.0f / 44100.0f;
    sf_engine_params_t params = {OSC_AUDIOIN, 1.0f, 0.6f, 0.0f, 0};   /* all metallic, no tone */
    int last_sign = 0, last_edge = -1, shortest = 1 << 30, longest = 0;
    for (int s = 0; s < 20000; ++s) {
        float out = sf_engine1_sample(&params, &voice, &voice);
        int sign = out > 0.0f ? 1 : -1;
        if (last_sign && sign != last_sign) {
            if (last_edge >= 0 && s > 1000) {
                int gap = s - last_edge;
                if (gap < shortest) shortest = gap;
                if (gap > longest) longest = gap;
            }
            last_edge = s;
        }
        last_sign = sign;
        voice.phase += voice.increment;
        if (voice.phase >= 1.0f) voice.phase -= 1.0f;
    }
    assert(longest - shortest <= 2);
}

/* A new note only marks the delay lines stale; each is cleared just before
 * an engine that uses it runs, and a legato note keeps what is in them. */
static void test_delay_lines_cleared_on_use(void) {
    unsigned age = 0;
    static sf_voice_t voice;
    memset(&voice, 0, sizeof(voice));
    sf_voice_start_params_t p = mono_params(&age, 1);
    sf_voice_start(&voice, 0, 48, 100, 0, 0, &p);
    for (int i = 0; i < SF_VOICE_DELAY; ++i) voice.delay1[i] = voice.delay2[i] = 0.5f;
    voice.last_env_out = 0.0f;   /* silent, so the next note starts at once */
    voice.delay_stale1 = voice.delay_stale2 = 0;
    sf_voice_start(&voice, 0, 50, 100, 0, 1, &p);
    assert(!voice.delay_stale1 && !voice.delay_stale2);
    sf_voice_start(&voice, 0, 52, 100, 0, 0, &p);
    assert(voice.delay_stale1 && voice.delay_stale2);
    sf_voice_prepare_delay(&voice, 0, 0);
    assert(voice.delay1[7] == 0.5f && voice.delay2[7] == 0.5f);
    sf_voice_prepare_delay(&voice, 0, 1);
    assert(voice.delay1[7] == 0.5f && voice.delay2[7] == 0.0f && !voice.delay_stale2);
    sf_voice_prepare_delay(&voice, 1, 1);
    assert(voice.delay1[SF_VOICE_DELAY - 1] == 0.0f && !voice.delay_stale1);
    assert(sf_engine1_uses_delay(OSC_KARPLUS) && !sf_engine1_uses_delay(OSC_VA));
    assert(sf_engine2_uses_delay(ENGINE_COMB) && sf_engine2_uses_delay(ENGINE_KARPLUS)
           && !sf_engine2_uses_delay(ENGINE_MULTIFILTER));
}

int main(void) {
    test_api_and_param_contract();
    test_device_page_layout();
    test_mod_rate_setting();
    test_audio_and_voice_lifecycle();
    test_effect_processing();
    test_fx_time_range_and_migration();
    test_envelope_lfo_ranges_and_migration();
    test_lfo_one_shot();
    test_vca_route_law();
    test_destination_scales();
    test_voice_steal_order();
    test_shaper_minifreak_mode();
    test_wavetable_modes();
    test_legacy_oscillator_state_migration();
    test_minifreak_style_engine_modes();
    test_vanalog_timbre_is_audible_at_shape_zero();
    test_fm_rm_modulates_from_muted_osc1();
    test_osc2_pitch_mod_and_chord_voicings();
    test_mod_quantize_steps_pitch_modulation();
    test_braids_ne_engines_and_steiner_filter();
    test_cycling_envelope();
    test_cycle_matrix_route_uses_source_level();
    test_reverb_tail();
    test_voice_sum_does_not_duck();
    test_all_engine_modes_render();
    test_factory_bank();
    test_preset_change_declick();
    test_imported_minifreak_bank();
    test_instance_pool();
    test_shaper_lfo();
    test_sequencer();
    test_seq_automation();
    test_macro_destination();
    test_cell_destinations();
    test_filter_fm();
    test_keyboard_chord_and_unison_modes();
    test_sustain_pedal();
    test_host_transport_lfo();
    test_seq_note_gets_cycle_steps();
    test_state_recall_resets_omitted_sections();
    test_reverb_to_delay_switch_is_clean();
    test_mono_env_reset_legato();
    test_lfo_div_accepts_index();
    test_wavetable_phase_wraps();
    test_seq_note_overflow();
    test_harmo_weight_cache();
    test_delay_engine_caches();
    test_engine_caches_match_uncached();
    test_wavefolder_folds_at_full_drive();
    test_speech_phoneme_ring_and_word_length();
    test_harm_high_note_sounds();
    test_audio_in_metallic_phase_runs_on();
    test_delay_lines_cleared_on_use();
    puts("Bizar plugin tests passed");
    return 0;
}
