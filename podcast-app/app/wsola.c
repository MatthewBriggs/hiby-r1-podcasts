/* wsola.c — pitch-preserving time stretch (see wsola.h).
 *
 * Each output hop, the next analysis window is searched within +/- SEEK_FRAMES
 * of its nominal position for the offset whose start best matches the tail we
 * are about to cross-fade into, then the two are Hann cross-faded. Matching uses
 * plain cross-correlation on a mono-mixed copy, which is cheap enough for a
 * 1 GHz MIPS core and good enough for speech.
 *
 * Sizes are in frames; a frame is `channels` interleaved samples.
 */

#include <stdlib.h>
#include <string.h>

#include "wsola.h"

#define WIN_MS      30      /* analysis window */
#define SEEK_MS     10      /* similarity search radius */
#define FIFO_FRAMES 65536   /* ~1.5 s at 44.1 kHz, plenty of slack */

static int   g_rate, g_ch;
static float g_speed = 1.0f;

static int16_t *g_fifo;      /* interleaved input ring */
static size_t   g_head, g_tail;   /* tail = write, head = read */

static int16_t *g_out;       /* pending output frames */
static size_t   g_out_len, g_out_pos;

static int16_t *g_tailbuf;   /* previous window's overlap tail */
static int      g_have_tail;

static int g_win, g_seek, g_hop_s;
static float *g_fade_in, *g_fade_out;

static size_t fifo_count(void) {
    return (g_tail - g_head + FIFO_FRAMES) % FIFO_FRAMES;
}

static void fifo_peek(size_t offset, int16_t *dst, size_t frames) {
    for (size_t i = 0; i < frames; i++) {
        size_t idx = (g_head + offset + i) % FIFO_FRAMES;
        for (int c = 0; c < g_ch; c++)
            dst[i * g_ch + c] = g_fifo[idx * g_ch + c];
    }
}

int wsola_init(int rate, int channels) {
    wsola_free();
    g_rate = rate > 0 ? rate : 44100;
    g_ch   = (channels == 1 || channels == 2) ? channels : 2;

    g_win   = g_rate * WIN_MS / 1000;
    g_seek  = g_rate * SEEK_MS / 1000;
    g_hop_s = g_win / 2;                    /* 50% overlap */

    g_fifo    = calloc(FIFO_FRAMES * g_ch, sizeof(int16_t));
    g_out     = calloc((size_t)g_win * g_ch, sizeof(int16_t));
    g_tailbuf = calloc((size_t)g_hop_s * g_ch, sizeof(int16_t));
    g_fade_in  = calloc(g_hop_s, sizeof(float));
    g_fade_out = calloc(g_hop_s, sizeof(float));
    if (!g_fifo || !g_out || !g_tailbuf || !g_fade_in || !g_fade_out) {
        wsola_free();
        return -1;
    }
    for (int i = 0; i < g_hop_s; i++) {
        /* Hann halves; equal-power enough for speech and cheap to build. */
        float t = (float)i / (float)(g_hop_s - 1);
        g_fade_in[i]  = t;
        g_fade_out[i] = 1.0f - t;
    }
    g_head = g_tail = 0;
    g_out_len = g_out_pos = 0;
    g_have_tail = 0;
    return 0;
}

void wsola_free(void) {
    free(g_fifo); free(g_out); free(g_tailbuf);
    free(g_fade_in); free(g_fade_out);
    g_fifo = NULL; g_out = NULL; g_tailbuf = NULL;
    g_fade_in = g_fade_out = NULL;
    g_head = g_tail = g_out_len = g_out_pos = 0;
    g_have_tail = 0;
}

void  wsola_set_speed(float s) { g_speed = (s < 0.5f) ? 0.5f : (s > 3.0f ? 3.0f : s); }
float wsola_speed(void)        { return g_speed; }

void wsola_reset(void) {
    g_head = g_tail = 0;
    g_out_len = g_out_pos = 0;
    g_have_tail = 0;
}

size_t wsola_pending(void) { return fifo_count(); }

size_t wsola_feed(const int16_t *in, size_t frames) {
    size_t space = FIFO_FRAMES - 1 - fifo_count();
    if (frames > space) frames = space;
    for (size_t i = 0; i < frames; i++) {
        size_t idx = (g_tail + i) % FIFO_FRAMES;
        for (int c = 0; c < g_ch; c++)
            g_fifo[idx * g_ch + c] = in[i * g_ch + c];
    }
    g_tail = (g_tail + frames) % FIFO_FRAMES;
    return frames;
}

/* Similarity between the stored tail and the candidate at `offset`. */
static long score_at(size_t offset) {
    long acc = 0;
    /* Subsample by 4: 4x cheaper, no audible difference for speech. */
    for (int i = 0; i < g_hop_s; i += 4) {
        size_t idx = (g_head + offset + i) % FIFO_FRAMES;
        int a = g_tailbuf[i * g_ch];
        int b = g_fifo[idx * g_ch];
        acc += (long)(a >> 8) * (b >> 8);
    }
    return acc;
}

static int produce(void) {
    size_t avail = fifo_count();
    size_t need = (size_t)g_win + (size_t)g_seek * 2 + 8;
    if (avail < need) return 0;

    size_t best = g_seek;                 /* nominal centre of the search */
    if (g_have_tail) {
        long best_score = -1;
        for (size_t off = 0; off <= (size_t)g_seek * 2; off += 2) {
            long sc = score_at(off);
            if (sc > best_score) { best_score = sc; best = off; }
        }
    }

    fifo_peek(best, g_out, (size_t)g_win);

    if (g_have_tail) {
        for (int i = 0; i < g_hop_s; i++)
            for (int c = 0; c < g_ch; c++) {
                float v = g_tailbuf[i * g_ch + c] * g_fade_out[i]
                        + g_out[i * g_ch + c]     * g_fade_in[i];
                if (v > 32767.f) v = 32767.f;
                if (v < -32768.f) v = -32768.f;
                g_out[i * g_ch + c] = (int16_t)v;
            }
    }

    /* Keep the second half as the tail for the next cross-fade. */
    memcpy(g_tailbuf, g_out + (size_t)g_hop_s * g_ch,
           (size_t)g_hop_s * g_ch * sizeof(int16_t));
    g_have_tail = 1;

    g_out_len = (size_t)g_hop_s;   /* only the faded part is final */
    g_out_pos = 0;

    /* Advance by the analysis hop only. `best` is a per-hop correction picked
     * from a window centred on g_seek; folding it into the pointer as well makes
     * it accumulate, which consumed input ~40% too fast (measured 1.75x when
     * 1.25x was asked for). */
    size_t hop_a = (size_t)((float)g_hop_s * g_speed + 0.5f);
    if (hop_a < 1) hop_a = 1;
    if (hop_a > avail) hop_a = avail;
    g_head = (g_head + hop_a) % FIFO_FRAMES;
    return 1;
}

size_t wsola_read(int16_t *out, size_t max_frames) {
    /* Passthrough at 1.0 keeps the common case bit-exact and cheap. */
    if (g_speed > 0.99f && g_speed < 1.01f) {
        size_t avail = fifo_count();
        size_t n = avail < max_frames ? avail : max_frames;
        if (n) {
            fifo_peek(0, out, n);
            g_head = (g_head + n) % FIFO_FRAMES;
            g_have_tail = 0;
        }
        return n;
    }

    size_t written = 0;
    while (written < max_frames) {
        if (g_out_pos >= g_out_len) {
            if (!produce()) break;
        }
        size_t n = g_out_len - g_out_pos;
        if (n > max_frames - written) n = max_frames - written;
        memcpy(out + written * g_ch,
               g_out + g_out_pos * g_ch,
               n * g_ch * sizeof(int16_t));
        g_out_pos += n;
        written += n;
    }
    return written;
}
