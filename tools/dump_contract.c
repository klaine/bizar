/*
 * dump_contract.c -- write the module's UI contract to two JSON files.
 *
 * The module serves `ui_hierarchy` and `chain_params` from get_param, so the
 * only drift-proof way to document them is to ask the plugin itself. The
 * build's --reference mode compiles this beside the DSP, runs it, and hands
 * the two files to scripts/generate_parameter_reference.py.
 *
 * Usage: dump_contract HIERARCHY_JSON CHAIN_PARAMS_JSON
 */
#include "plugin_api_v2_compat.h"

#include <stdio.h>
#include <string.h>

static int write_document(plugin_api_v2_t *api, void *instance, const char *key,
                          const char *path) {
    static char buffer[65536];
    int n = api->get_param(instance, key, buffer, (int)sizeof(buffer));
    if (n <= 0) {
        fprintf(stderr, "dump_contract: %s read failed\n", key);
        return 0;
    }
    FILE *file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "dump_contract: cannot write %s\n", path);
        return 0;
    }
    size_t written = fwrite(buffer, 1, (size_t)n, file);
    fclose(file);
    if (written != (size_t)n) {
        fprintf(stderr, "dump_contract: short write to %s\n", path);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr,
                "usage: dump_contract HIERARCHY_JSON CHAIN_PARAMS_JSON\n");
        return 2;
    }

    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    if (!api) {
        fprintf(stderr, "dump_contract: plugin init failed\n");
        return 1;
    }
    void *instance = api->create_instance("", "{}");
    if (!instance) {
        fprintf(stderr, "dump_contract: create_instance failed\n");
        return 1;
    }

    int ok = write_document(api, instance, "ui_hierarchy", argv[1])
          && write_document(api, instance, "chain_params_full", argv[2]);

    api->destroy_instance(instance);
    return ok ? 0 : 1;
}
