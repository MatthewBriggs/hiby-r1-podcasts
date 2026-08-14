/* eq.c — 10-band parametric EQ. See eq.h for the design rationale.
 *
 * Band parameters (type/Fc/gain/Q, what the UI edits and what an
 * EqualizerAPO file describes) and the coefficients derived from them are
 * shared between the UI thread and the audio thread, so they sit behind
 * g_lock. Filter memory (the DF2T z1/z2 state) is not shared -- only the
 * audio thread ever touches it, both to run the cascade and to clear it on
 * a format change -- so it lives outside the lock entirely.
 */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "eq.h"

typedef struct { float b0, b1, b2, a1, a2; } coeffs_t;
typedef struct { float z1, z2; } state_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int        g_enabled;
static unsigned    g_rate = 44100;
static eq_band_t   g_band[EQ_MAX_BANDS];
static int         g_band_n;
static float        g_preamp_db;
static float        g_preamp_lin = 1.0f;
static char         g_name[EQ_NAME_LEN];
static coeffs_t      g_coeffs[EQ_MAX_BANDS];

static state_t g_state[EQ_MAX_BANDS][EQ_MAX_CH];   /* audio-thread only */

/* RBJ Audio-EQ-Cookbook. Same formulas verified against the real Sony
 * WH-1000XM4 filter set in the on-device benchmark this was built from. */
static void design(coeffs_t *c, eq_type_t type, double rate, double fc,
                   double gain_db, double Q) {
    double A = pow(10.0, gain_db / 40.0);
    double w0 = 2.0 * M_PI * fc / rate;
    double cw = cos(w0), sw = sin(w0);
    double alpha = sw / (2.0 * Q);
    double b0, b1, b2, a0, a1, a2;
    if (type == EQ_PEAK) {
        b0 = 1 + alpha * A;  b1 = -2 * cw;  b2 = 1 - alpha * A;
        a0 = 1 + alpha / A;  a1 = -2 * cw;  a2 = 1 - alpha / A;
    } else {
        double sq = sqrt(A) * 2.0 * alpha;
        if (type == EQ_LOW_SHELF) {
            b0 =    A * ((A + 1) - (A - 1) * cw + sq);
            b1 =  2 * A * ((A - 1) - (A + 1) * cw);
            b2 =    A * ((A + 1) - (A - 1) * cw - sq);
            a0 =        (A + 1) + (A - 1) * cw + sq;
            a1 =   -2 * ((A - 1) + (A + 1) * cw);
            a2 =        (A + 1) + (A - 1) * cw - sq;
        } else {
            b0 =    A * ((A + 1) + (A - 1) * cw + sq);
            b1 = -2 * A * ((A - 1) + (A + 1) * cw);
            b2 =    A * ((A + 1) + (A - 1) * cw - sq);
            a0 =        (A + 1) - (A - 1) * cw + sq;
            a1 =    2 * ((A - 1) - (A + 1) * cw);
            a2 =        (A + 1) - (A - 1) * cw - sq;
        }
    }
    c->b0 = (float)(b0 / a0); c->b1 = (float)(b1 / a0); c->b2 = (float)(b2 / a0);
    c->a1 = (float)(a1 / a0); c->a2 = (float)(a2 / a0);
}

/* Caller holds g_lock. Cheap enough (ten sin/cos/pow calls at most) to just
 * redo the whole set rather than track which single band changed. */
static void recompute_locked(void) {
    for (int i = 0; i < g_band_n; i++)
        design(&g_coeffs[i], g_band[i].type, g_rate, g_band[i].fc,
              g_band[i].gain_db, g_band[i].q);
    g_preamp_lin = powf(10.0f, g_preamp_db / 20.0f);
}

void eq_set_enabled(int on) { g_enabled = on; }
int  eq_enabled(void)       { return g_enabled; }

void eq_set_profile(const eq_profile_t *p) {
    pthread_mutex_lock(&g_lock);
    snprintf(g_name, sizeof(g_name), "%s", p->name);
    g_preamp_db = p->preamp_db;
    g_band_n = p->band_n > EQ_MAX_BANDS ? EQ_MAX_BANDS : p->band_n;
    memcpy(g_band, p->band, sizeof(g_band[0]) * (size_t)g_band_n);
    recompute_locked();
    pthread_mutex_unlock(&g_lock);
}

void eq_get_profile(eq_profile_t *out) {
    pthread_mutex_lock(&g_lock);
    snprintf(out->name, sizeof(out->name), "%s", g_name);
    out->preamp_db = g_preamp_db;
    out->band_n = g_band_n;
    memcpy(out->band, g_band, sizeof(out->band[0]) * (size_t)g_band_n);
    pthread_mutex_unlock(&g_lock);
}

void eq_set_band(int i, const eq_band_t *b) {
    if (i < 0 || i >= EQ_MAX_BANDS) return;
    pthread_mutex_lock(&g_lock);
    g_band[i] = *b;
    if (i >= g_band_n) g_band_n = i + 1;
    recompute_locked();
    pthread_mutex_unlock(&g_lock);
}

void eq_set_preamp(float db) {
    pthread_mutex_lock(&g_lock);
    g_preamp_db = db;
    g_preamp_lin = powf(10.0f, db / 20.0f);
    pthread_mutex_unlock(&g_lock);
}

void eq_set_format(unsigned rate, int channels) {
    (void)channels;
    pthread_mutex_lock(&g_lock);
    g_rate = rate ? rate : 44100;
    recompute_locked();
    pthread_mutex_unlock(&g_lock);
    /* Filter memory is audio-thread-only, and eq_set_format is always called
     * from that thread (audio.c, at track open and at a gapless format
     * change) -- no lock needed to clear it. A rate change makes the old
     * state meaningless regardless (it was accumulated on a different time
     * base), so this is required, not just tidy. */
    memset(g_state, 0, sizeof(g_state));
}

static inline float biquad(const coeffs_t *c, state_t *z, float x) {
    float y = c->b0 * x + z->z1;
    z->z1 = c->b1 * x - c->a1 * y + z->z2;
    z->z2 = c->b2 * x - c->a2 * y;
    return y;
}

/* Snapshot the shared parameters once per call (once per chunk in practice,
 * a few thousand frames), not once per sample -- the lock is held only long
 * enough to memcpy a few hundred bytes. */
typedef struct {
    coeffs_t c[EQ_MAX_BANDS];
    int      on[EQ_MAX_BANDS];
    int      n;
    float    preamp;
} snapshot_t;

static void take_snapshot(snapshot_t *s) {
    pthread_mutex_lock(&g_lock);
    s->n = g_band_n;
    s->preamp = g_preamp_lin;
    memcpy(s->c, g_coeffs, sizeof(s->c[0]) * (size_t)s->n);
    for (int i = 0; i < s->n; i++) s->on[i] = g_band[i].on;
    pthread_mutex_unlock(&g_lock);
}

void eq_process_s16(short *buf, int frames, int channels) {
    if (!g_enabled) return;
    if (channels > EQ_MAX_CH) channels = EQ_MAX_CH;
    snapshot_t s;
    take_snapshot(&s);
    for (int f = 0; f < frames; f++) {
        for (int ch = 0; ch < channels; ch++) {
            int idx = f * channels + ch;
            float x = (float)buf[idx] * (1.0f / 32768.0f) * s.preamp;
            for (int b = 0; b < s.n; b++)
                if (s.on[b]) x = biquad(&s.c[b], &g_state[b][ch], x);
            float y = x * 32768.0f;
            if (y > 32767.0f) y = 32767.0f;
            else if (y < -32768.0f) y = -32768.0f;
            buf[idx] = (short)(y >= 0 ? y + 0.5f : y - 0.5f);
        }
    }
}

/* DTFT of the cascade at z = e^jw: multiply each band's |H| (sum the dB,
 * equivalently), plus the preamp as a flat offset. Ten bands times a few
 * dozen points is a handful of trig calls, done once per screen redraw, not
 * once per sample -- nowhere near the same budget as eq_process_*. */
void eq_response_db(const float *freq_hz, float *out_db, int n) {
    snapshot_t s;
    take_snapshot(&s);
    double preamp_db = 20.0 * log10(s.preamp > 1e-9 ? s.preamp : 1e-9);
    for (int i = 0; i < n; i++) {
        double w = 2.0 * M_PI * freq_hz[i] / 44100.0;
        double cw = cos(w), sw = sin(w), c2w = cos(2 * w), s2w = sin(2 * w);
        double db = preamp_db;
        for (int b = 0; b < s.n; b++) {
            if (!s.on[b]) continue;
            const coeffs_t *c = &s.c[b];
            double nr = c->b0 + c->b1 * cw + c->b2 * c2w;
            double ni =       -c->b1 * sw - c->b2 * s2w;
            double dr = 1.0   + c->a1 * cw + c->a2 * c2w;
            double di =       -c->a1 * sw - c->a2 * s2w;
            double magd = sqrt(dr * dr + di * di);
            if (magd < 1e-9) magd = 1e-9;
            db += 20.0 * log10(sqrt(nr * nr + ni * ni) / magd);
        }
        out_db[i] = (float)db;
    }
}

void eq_process_s32(int32_t *buf, int frames, int channels) {
    if (!g_enabled) return;
    if (channels > EQ_MAX_CH) channels = EQ_MAX_CH;
    snapshot_t s;
    take_snapshot(&s);
    for (int f = 0; f < frames; f++) {
        for (int ch = 0; ch < channels; ch++) {
            int idx = f * channels + ch;
            float x = (float)buf[idx] * (1.0f / 2147483648.0f) * s.preamp;
            for (int b = 0; b < s.n; b++)
                if (s.on[b]) x = biquad(&s.c[b], &g_state[b][ch], x);
            float y = x * 2147483648.0f;
            if (y > 2147483520.0f) y = 2147483520.0f;
            else if (y < -2147483648.0f) y = -2147483648.0f;
            buf[idx] = (int32_t)y;
        }
    }
}
