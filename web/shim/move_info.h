/*
 * Browser stand-in for include/move_info.h. There is no Move set to read in a
 * web page, so the tempo comes from a global the page sets through
 * sf_web_set_tempo(). Shadows the real header via -Iweb/shim.
 */
#ifndef MOVE_INFO_H
#define MOVE_INFO_H

#include <stdint.h>

typedef struct {
    uint8_t valid;
    float tempo;
} move_info_t;

extern float sf_web_tempo;

static inline int move_info_read(move_info_t *out)
{
    out->valid = 1;
    out->tempo = sf_web_tempo;
    return 1;
}

#endif
