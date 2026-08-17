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

/* MSEB: HiBy's named tuning bands (see the Rockbox forum thread this was
 * transcribed from -- 9 fixed-frequency, fixed-Q parametric bands, each
 * describing what it does rather than its Hz, with only gain left free).
 * Unlike the profile above, Fc/type/Q are not user data -- they are this
 * fixed table, the actual specification, not tunable. */
#define MSEB_BAND_N 9

typedef struct {
    const char *name;         /* "Bass extension" */
    const char *freq_label;   /* "70 Hz shelf", for the UI subtitle */
    eq_type_t   type;
    float       fc;
    float       q;
} mseb_band_def_t;

extern const mseb_band_def_t EQ_MSEB_BANDS[MSEB_BAND_N];

/* MSEB and the profile can both be active. Rather than run both in full --
 * which would need EQ_MAX_BANDS raised from 10 to as many as 19 and roughly
 * double the cascade's measured CPU cost (8.5%->17% at 44.1kHz, 18.4%->37%
 * at 96kHz, this file's own header) -- the total stays capped at
 * EQ_MAX_BANDS. When MSEB is on, its 9 bands take the first slots and the
 * profile is truncated to whatever is left (one band, today): MSEB wins the
 * budget, chosen deliberately over doubling a cost this file has already
 * measured once, on the same code path involved in BG32. */
void eq_set_mseb(int enabled, const float gain_db[MSEB_BAND_N]);
void eq_get_mseb(int *enabled, float gain_db[MSEB_BAND_N]);
int  eq_mseb_enabled(void);

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

/* Diagnostics for the sustained-tone report (BG32).
 *
 * Two failed fixes went in on reasoning alone -- a runaway-state reset that
 * fired silently, so there was never any way to tell whether it had fired at
 * all, and a threshold picked without checking what magnitude is already
 * audible. The point of these counters is to make the next occurrence produce
 * evidence instead of another hypothesis, and specifically to answer the one
 * question that splits the search in half: is the signal leaving this filter
 * already wrong, or is it clean and something downstream (bluealsa, the
 * headset, the A2DP link) is mangling it?
 *
 * `clipped` and `peak` answer that. A sustained tone of our own making means
 * `peak` pinned high and `clipped` counting a large fraction of `samples`;
 * a clean output with the user still hearing a tone rules this file out
 * entirely. `zmax` shows filter memory growing even while it stays under the
 * reset threshold -- the case the previous fix was blind to.
 *
 * The audio thread accumulates; audio.c drains on its own periodic tick.
 * Same thread does both, so there is no lock here and none needed. */
/* Peak and clipping are per channel, not aggregated. The reported symptom has
 * always been the left ear specifically, and both channels run identical
 * coefficients -- so left/right asymmetry in our own output is the one
 * measurement that separates "this filter is producing the tone" from "our
 * output is clean and something downstream (SBC's joint stereo, the A2DP
 * link, the headset) is". Aggregated figures cannot answer that at all. */
typedef struct {
    unsigned long samples;              /* frames processed since last drain */
    unsigned long clipped[EQ_MAX_CH];   /* per channel, hit the output clamp */
    unsigned long trips;                /* sanitize_state() resets */
    float         peak[EQ_MAX_CH];      /* per channel, normalised, pre-clamp */
    float         zmax;                 /* largest |z1|/|z2| across all bands */
    int           trip_band;            /* band and channel of the most recent */
    int           trip_ch;              /* trip, or -1 when nothing tripped */
} eq_stats_t;

/* Copy the accumulated counters out and reset them. */
void eq_stats_drain(eq_stats_t *out);

#endif
