/*
 * Minimal Schwung Plugin API v2 ABI declarations.
 *
 * The definitions below match the public declarations in
 * charlesvestal/schwung src/host/plugin_api_v1.h (MIT, see
 * THIRD_PARTY_NOTICES.md). host_api_v1_t is declared in full so the plugin can
 * read the host's tempo, transport and beat position; every field is guarded
 * by a NULL check at the call site, and the host passes NULL in the browser
 * build. Do not add fields: a module cannot extend this struct from its side.
 */
#ifndef BIZAR_PLUGIN_API_V2_COMPAT_H
#define BIZAR_PLUGIN_API_V2_COMPAT_H

#include <stddef.h>
#include <stdint.h>

#define MOVE_PLUGIN_API_VERSION_2 2

#define MOVE_CLOCK_STATUS_UNAVAILABLE 0
#define MOVE_CLOCK_STATUS_STOPPED 1
#define MOVE_CLOCK_STATUS_RUNNING 2

typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
    int (*get_clock_status)(void);
    void *mod_emit_value;
    void *mod_clear_source;
    void *mod_host_ctx;
    float (*get_bpm)(void);
    int (*midi_inject_to_move)(const uint8_t *msg, int len);
    int (*slot_recv_channel)(void *instance);
    /* Beats since transport start, < 0 when no transport runs. May be NULL. */
    double (*get_beat_position)(void);
    /* Reserved run of NULLs: never consume it (see Schwung's header). */
    void *reserved[8];
} host_api_v1_t;

#if defined(__aarch64__) || defined(__x86_64__)
_Static_assert(offsetof(host_api_v1_t, reserved) == 120,
               "host_api_v1_t layout drifted from Schwung's");
#endif

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void *(*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host);

#endif
