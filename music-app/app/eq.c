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

/* The actual MSEB specification, transcribed from the Rockbox forum thread
 * (HiBy R1 topic, post #49, updated with the engineer's own figures). Fc and
 * Q are fixed by that spec, not editable -- only gain_db varies, driven by
 * eq_set_mseb() below. */
const mseb_band_def_t EQ_MSEB_BANDS[MSEB_BAND_N] = {
    { "Bass extension",    "70 Hz shelf",   EQ_LOW_SHELF,   70.0f, 0.7071f },
    { "Bass texture",      "100 Hz",        EQ_PEAK,       100.0f, 0.85f   },
    { "Note thickness",    "200 Hz",        EQ_PEAK,       200.0f, 0.6667f },
    { "Voice",             "650 Hz",        EQ_PEAK,       650.0f, 0.4f    },
    { "Female overtones",  "3.0 kHz",       EQ_PEAK,      3000.0f, 1.414f  },
    { "Sibilance LF",      "5.8 kHz",       EQ_PEAK,      5800.0f, 1.0f    },
    { "Sibilance HF",      "9.2 kHz",       EQ_PEAK,      9200.0f, 1.0f    },
    { "Impulse response",  "7.5 kHz",       EQ_PEAK,      7500.0f, 0.4f    },
    { "Air",               "10 kHz shelf",  EQ_HIGH_SHELF, 10000.0f, 0.7071f },
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int        g_enabled;
static unsigned    g_rate = 44100;
/* The profile's own bands -- what SC_EQ_BAND edits and what ep_save() writes.
 * Separate from g_band[] below, which is the merged/active cascade actually
 * fed to the biquads: g_band is derived from this plus g_mseb_*, not edited
 * directly, since MSEB and the profile both contribute to it. */
static eq_band_t   g_pband[EQ_MAX_BANDS];
static int         g_pband_n;
static float        g_preamp_db;
static float        g_preamp_lin = 1.0f;
static char         g_name[EQ_NAME_LEN];

static float       g_mseb_gain[MSEB_BAND_N];
static int         g_mseb_on;

static eq_band_t   g_band[EQ_MAX_BANDS];    /* active cascade -- see recompute_locked() */
static int         g_band_n;
static coeffs_t      g_coeffs[EQ_MAX_BANDS];

static state_t g_state[EQ_MAX_BANDS][EQ_MAX_CH];   /* audio-thread only */

/* See eq.h. Audio-thread only, same as g_state -- accumulated in the process
 * loops, drained by audio.c from that same thread, so no lock. */
static eq_stats_t g_st = { 0, { 0, 0 }, 0, { 0.0f, 0.0f }, 0.0f, -1, -1 };

void eq_stats_drain(eq_stats_t *out) {
    if (out) *out = g_st;
    g_st.samples = 0; g_st.trips = 0; g_st.zmax = 0.0f;
    for (int c = 0; c < EQ_MAX_CH; c++) { g_st.clipped[c] = 0; g_st.peak[c] = 0.0f; }
    g_st.trip_band = -1; g_st.trip_ch = -1;
}

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
 * redo the whole set rather than track which single band changed.
 *
 * Builds the ACTIVE cascade (g_band[]/g_band_n/g_coeffs[]) from the profile
 * (g_pband[]) and MSEB (g_mseb_gain[]) together, rather than editing g_band
 * directly the way earlier versions of this file did. When MSEB is on, its 9
 * bands are written first and always fit (9 < EQ_MAX_BANDS); the profile
 * then gets whatever budget remains, truncated rather than merged -- see
 * eq.h's comment on eq_set_mseb() for why a hard cap was chosen over raising
 * EQ_MAX_BANDS to run both in full. */
static void recompute_locked(void) {
    g_band_n = 0;
    if (g_mseb_on) {
        for (int i = 0; i < MSEB_BAND_N && g_band_n < EQ_MAX_BANDS; i++) {
            eq_band_t *b = &g_band[g_band_n++];
            b->type = EQ_MSEB_BANDS[i].type;
            b->fc   = EQ_MSEB_BANDS[i].fc;
            b->q    = EQ_MSEB_BANDS[i].q;
            b->gain_db = g_mseb_gain[i];
            b->on = 1;
        }
    }
    int room = EQ_MAX_BANDS - g_band_n;
    int take = g_pband_n < room ? g_pband_n : room;
    memcpy(&g_band[g_band_n], g_pband, sizeof(g_band[0]) * (size_t)take);
    g_band_n += take;

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
    g_pband_n = p->band_n > EQ_MAX_BANDS ? EQ_MAX_BANDS : p->band_n;
    memcpy(g_pband, p->band, sizeof(g_pband[0]) * (size_t)g_pband_n);
    recompute_locked();
    pthread_mutex_unlock(&g_lock);
}

void eq_get_profile(eq_profile_t *out) {
    pthread_mutex_lock(&g_lock);
    snprintf(out->name, sizeof(out->name), "%s", g_name);
    out->preamp_db = g_preamp_db;
    out->band_n = g_pband_n;
    memcpy(out->band, g_pband, sizeof(out->band[0]) * (size_t)g_pband_n);
    pthread_mutex_unlock(&g_lock);
}

void eq_set_band(int i, const eq_band_t *b) {
    if (i < 0 || i >= EQ_MAX_BANDS) return;
    pthread_mutex_lock(&g_lock);
    g_pband[i] = *b;
    if (i >= g_pband_n) g_pband_n = i + 1;
    recompute_locked();
    pthread_mutex_unlock(&g_lock);
}

void eq_set_mseb(int enabled, const float gain_db[MSEB_BAND_N]) {
    pthread_mutex_lock(&g_lock);
    g_mseb_on = enabled;
    if (gain_db) memcpy(g_mseb_gain, gain_db, sizeof(g_mseb_gain));
    recompute_locked();
    pthread_mutex_unlock(&g_lock);
}

void eq_get_mseb(int *enabled, float gain_db[MSEB_BAND_N]) {
    pthread_mutex_lock(&g_lock);
    if (enabled) *enabled = g_mseb_on;
    if (gain_db) memcpy(gain_db, g_mseb_gain, sizeof(g_mseb_gain[0]) * MSEB_BAND_N);
    pthread_mutex_unlock(&g_lock);
}

int eq_mseb_enabled(void) { return g_mseb_on; }

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

/* Defensive: an IIR filter's own state can in principle run away (a marginal
 * pole from an extreme gain/Q combination, or -- this core is soft-float, see
 * audio.c's own CPU-measurement comment -- an edge case in denormal
 * handling), and z1/z2 carry over between calls indefinitely with nothing
 * that would otherwise ever reset a bad value short of a format change.
 * Checked once per buffer, not per sample: negligible cost, and normal audio
 * in this +/-1.0 domain never comes remotely close to this magnitude.
 *
 * BG32 (an occasional loud tone in one channel that pausing and resuming
 * cleared) is why the threshold moved from 1e6f to 8.0f. Be clear about what
 * that change is worth: 1e6f was plainly useless, since the output is clamped
 * on every sample regardless of z1/z2 (see the write side below), so a state
 * of z=3 already produces a continuously clipped full-scale tone while
 * sitting six orders of magnitude below the old trigger. But 8.0f does not
 * fix BG32 either -- it was picked as "headroom" without working out what
 * magnitude is already audible, and in a +/-1.0 domain anything past roughly
 * z=1 is. An oscillation parked between 1 and 8 is a full-volume tone that
 * still walks straight through this check.
 *
 * It was left at 8.0f rather than tightened further because a legitimate
 * filter ringing on loud material does reach past 1.0, and a reset fires a
 * discontinuity into the signal -- a click. Tightening blindly trades one
 * audible defect for another. The counters below exist to settle it with
 * evidence: BG32 recurred with this threshold live, so either the state never
 * gets near 8.0 (zmax will say so, and the cause is elsewhere) or it does and
 * resets are firing constantly (trips will say so). Until one of those
 * numbers comes back from a real occurrence, this stays a safety net of
 * unproven value, not a fix. */
static void sanitize_state(int n, int channels) {
    for (int b = 0; b < n; b++)
        for (int ch = 0; ch < channels; ch++) {
            state_t *z = &g_state[b][ch];
            /* NaN fails every comparison, so it never raises zmax -- that is
             * fine, the trip counter catches it and zmax is here to show
             * growth that stays *under* the threshold. */
            float a1 = fabsf(z->z1), a2 = fabsf(z->z2);
            float m = a1 > a2 ? a1 : a2;
            if (m > g_st.zmax) g_st.zmax = m;
            if (!isfinite(z->z1) || !isfinite(z->z2) ||
                a1 > 8.0f || a2 > 8.0f) {
                g_st.trips++;
                g_st.trip_band = b; g_st.trip_ch = ch;
                z->z1 = 0.0f; z->z2 = 0.0f;
            }
        }
}

/* How often sanitize_state() runs within a buffer, in frames -- a tradeoff
 * between the cost of the check (negligible either way, see sanitize_state's
 * own comment) and how long a runaway state gets to sit at an audible,
 * clamped-full-scale amplitude before it's caught. At 256 this is ~5.8ms at
 * 44.1kHz, worst case, instead of the full ~46ms chunk (audio.c's
 * CHUNK_FRAMES=2048) a once-per-buffer check would allow -- worth the extra
 * handful of comparisons per buffer given what the failure sounds like. */
#define SANITIZE_PERIOD 256

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
            /* Measured on the normalised value, before the clamp, so a
             * runaway shows its real magnitude rather than the clipped one. */
            float ax = fabsf(x);
            if (ax > g_st.peak[ch]) g_st.peak[ch] = ax;
            float y = x * 32768.0f;
            if (y > 32767.0f) { y = 32767.0f; g_st.clipped[ch]++; }
            else if (y < -32768.0f) { y = -32768.0f; g_st.clipped[ch]++; }
            buf[idx] = (short)(y >= 0 ? y + 0.5f : y - 0.5f);
        }
        g_st.samples++;   /* frames, so it is the denominator for clipped[] */
        if ((f % SANITIZE_PERIOD) == SANITIZE_PERIOD - 1)
            sanitize_state(s.n, channels);
    }
    sanitize_state(s.n, channels);
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
            float ax = fabsf(x);
            if (ax > g_st.peak[ch]) g_st.peak[ch] = ax;
            float y = x * 2147483648.0f;
            if (y > 2147483520.0f) { y = 2147483520.0f; g_st.clipped[ch]++; }
            else if (y < -2147483648.0f) { y = -2147483648.0f; g_st.clipped[ch]++; }
            buf[idx] = (int32_t)y;
        }
        g_st.samples++;   /* frames, so it is the denominator for clipped[] */
        if ((f % SANITIZE_PERIOD) == SANITIZE_PERIOD - 1)
            sanitize_state(s.n, channels);
    }
    sanitize_state(s.n, channels);
}
