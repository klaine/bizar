/*
 * Minimal Schwung Plugin API v2 ABI declarations.
 *
 * The definitions below match the public v2 declarations in
 * charlesvestal/schwung src/host/plugin_api_v1.h. The plugin does not access
 * fields in host_api_v1_t, so that type is intentionally opaque here.
 */
#ifndef BIZAR_PLUGIN_API_V2_COMPAT_H
#define BIZAR_PLUGIN_API_V2_COMPAT_H

#include <stdint.h>

#define MOVE_PLUGIN_API_VERSION_2 2

typedef struct host_api_v1 host_api_v1_t;

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
