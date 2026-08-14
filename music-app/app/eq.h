/* eq.h — 10-band parametric EQ, EqualizerAPO-compatible parameters.
 *
 * Biquads in Direct Form II Transposed (RBJ Audio-EQ-Cookbook formulas),
 * plain float: measured on-device, this CPU's FPU is real (float math ran at
 * 2.3x integer speed in a microbenchmark, not the 20-100x+ a software-
 * emulated FPU would cost), so there is no need for fixed-point here the way
 * there was for nothing else in this codebase -- everything else avoiding
 * float predates that measurement. The full 10-band cascade measured 8.5% of
 * one core at 44.1 kHz and 18.4% at 96 kHz, stacked on top of decode.
 */
#ifndef EQ_H
#define EQ_H
#include <stdint.h>

#define EQ_MAX_BANDS 10
#define EQ_MAX_CH    2
#define EQ_NAME_LEN  64

typedef enum { EQ_LOW_SHELF = 0, EQ_PEAK = 1, EQ_HIGH_SHELF = 2 } eq_type_t;

typedef struct {
    eq_type_t type;
    int   on;
    float fc;         /* Hz */
    float gain_db;
    float q;
} eq_band_t;

typedef struct {
    char      name[EQ_NAME_LEN];
    float     preamp_db;
    eq_band_t band[EQ_MAX_BANDS];
    int       band_n;
} eq_profile_t;

void eq_set_enabled(int on);
int  eq_enabled(void);

/* Replaces the whole profile -- after an import or switching profiles. */
void eq_set_profile(const eq_profile_t *p);
void eq_get_profile(eq_profile_t *out);

/* Live edit of one band or the preamp, e.g. while a slider is being dragged.
 * Recomputes only that band's coefficients; does not reset filter memory, so
 * (like every real-time parametric EQ) a large jump mid-playback can click --
 * normal, not a bug. */
void eq_set_band(int i, const eq_band_t *b);
void eq_set_preamp(float db);

/* Call whenever the decode rate or channel count changes, gapless boundaries
 * included: biquad coefficients are rate-dependent, so skipping this after a
 * rate change leaves every band's cutoff wrong until the next call. Also
 * clears filter memory, which a rate change must do anyway (old state was
 * computed on a different time base). Channels above EQ_MAX_CH are clamped. */
void eq_set_format(unsigned rate, int channels);

/* Apply in place, interleaved, `channels` matching the last eq_set_format
 * call. Both are a fast no-op (a single enabled check) when disabled. */
void eq_process_s16(short *buf, int frames, int channels);
/* Same shape dr_flac/dr_wav hand back: left-aligned in 32 bits. Applied
 * before the existing S24/S32 shift-and-dither step in audio.c. */
void eq_process_s32(int32_t *buf, int frames, int channels);

/* Magnitude response in dB at each of `n` frequencies (Hz), for the overview
 * screen's curve. Evaluated at a fixed 44100 Hz reference regardless of what
 * is currently playing -- a UI curve needs the shape, not a per-track exact
 * answer, and it barely moves across the rates this app plays. Includes the
 * preamp and every band whose `on` is set, the same as eq_process_* respects. */
void eq_response_db(const float *freq_hz, float *out_db, int n);

#endif
