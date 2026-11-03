#ifndef BIZAR_DSP_COMMON_H
#define BIZAR_DSP_COMMON_H

/* Constants and small helpers shared by the DSP sources. */

#include <math.h>
#include <stdint.h>

/* Move runs at a fixed 44.1 kHz; every coefficient in the module assumes it. */
#define SF_SAMPLE_RATE 44100.0f
#define SF_PI 3.14159265358979323846f
#define SF_TWO_PI (2.0f * SF_PI)

/* Clamp to lo..hi, for finite lo <= hi. A non-finite value (NaN or inf)
 * lands on lo, so a bad control can never reach the audio path. Written as two
 * comparisons rather than with isfinite, which compilers expand into about 20
 * integer instructions: NaN fails both comparisons, +inf is the one value
 * above hi that is not clamped to it, and -inf fails x >= lo. */
static inline float sf_clampf(float x, float lo, float hi) {
    if (x > hi) return x == INFINITY ? lo : hi;
    return x >= lo ? x : lo;
}

/* xorshift32. A zero state would stay zero forever, so it is reseeded. */
static inline uint32_t sf_xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x ? x : 0x9e3779b9u;
    return *state;
}

#endif
