/* alsa_bench.c — RP7 compiler/libc comparison. Deliberately small: open the
 * real output device the same way audio.c's pcm_open() does (dlopen'd
 * libasound, plughw:0,0, S16_LE/44100/2ch), write one second of a 440Hz
 * tone, close. Prints wall-clock timings to stdout so three builds (this
 * toolchain's glibc target, its musl target, and the vendor SDK's own
 * mips-linux-gnu-gcc) can be compared on the same real hardware without
 * dragging in SQLite/codecs/the rest of the app.
 *
 * Same dlopen'd-symbol pattern audio.c already uses (SYM macro) rather than
 * linking -lasound directly -- proven working on this exact device already,
 * no reason to diverge from it for a benchmark that is supposed to measure
 * the app's own real path.
 */
#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define SND_PCM_STREAM_PLAYBACK 0
#define SND_PCM_ACCESS_RW_INTERLEAVED 3
#define SND_PCM_FORMAT_S16_LE 2

typedef long snd_pcm_sframes_t;
typedef unsigned long snd_pcm_uframes_t;

static void *g_alsa;
static int (*x_open)(void **, const char *, int, int);
static int (*x_close)(void *);
static snd_pcm_sframes_t (*x_writei)(void *, const void *, snd_pcm_uframes_t);
static int (*x_hwp_malloc)(void **);
static void (*x_hwp_free)(void *);
static int (*x_hwp_any)(void *, void *);
static int (*x_hwp_set_access)(void *, void *, int);
static int (*x_hwp_set_format)(void *, void *, int);
static int (*x_hwp_set_channels)(void *, void *, unsigned);
static int (*x_hwp_set_rate_near)(void *, void *, unsigned *, int *);
static int (*x_hwp_apply)(void *, void *);
static int (*x_prepare)(void *);

#define SYM(p, name) do { \
    *(void **)(&p) = dlsym(g_alsa, name); \
    if (!p) { fprintf(stderr, "missing %s\n", name); return -1; } \
} while (0)

static int load_libs(void) {
    g_alsa = dlopen("libasound.so.2", RTLD_LAZY);
    if (!g_alsa) g_alsa = dlopen("libasound.so", RTLD_LAZY);
    if (!g_alsa) { fprintf(stderr, "no libasound: %s\n", dlerror()); return -1; }
    SYM(x_open, "snd_pcm_open");
    SYM(x_close, "snd_pcm_close");
    SYM(x_writei, "snd_pcm_writei");
    SYM(x_hwp_malloc, "snd_pcm_hw_params_malloc");
    SYM(x_hwp_free, "snd_pcm_hw_params_free");
    SYM(x_hwp_any, "snd_pcm_hw_params_any");
    SYM(x_hwp_set_access, "snd_pcm_hw_params_set_access");
    SYM(x_hwp_set_format, "snd_pcm_hw_params_set_format");
    SYM(x_hwp_set_channels, "snd_pcm_hw_params_set_channels");
    SYM(x_hwp_set_rate_near, "snd_pcm_hw_params_set_rate_near");
    SYM(x_hwp_apply, "snd_pcm_hw_params");
    SYM(x_prepare, "snd_pcm_prepare");
    return 0;
}

static double ms_since(struct timespec *t0) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0->tv_sec) * 1000.0 + (t1.tv_nsec - t0->tv_nsec) / 1e6;
}

int main(void) {
    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    if (load_libs() < 0) return 1;
    double t_dlopen = ms_since(&t_start);

    void *pcm = NULL;
    if (x_open(&pcm, "plughw:0,0", SND_PCM_STREAM_PLAYBACK, 0) < 0 || !pcm) {
        fprintf(stderr, "snd_pcm_open failed\n");
        return 1;
    }
    double t_open = ms_since(&t_start);

    void *hw = NULL;
    x_hwp_malloc(&hw);
    x_hwp_any(pcm, hw);
    x_hwp_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    x_hwp_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
    x_hwp_set_channels(pcm, hw, 2);
    unsigned rate = 44100;
    x_hwp_set_rate_near(pcm, hw, &rate, NULL);
    x_hwp_apply(pcm, hw);
    x_hwp_free(hw);
    x_prepare(pcm);
    double t_ready = ms_since(&t_start);

    /* One second of a 440Hz tone, S16_LE stereo, written in 4096-frame
     * chunks -- representative of the real decode-then-write loop's own
     * chunk size, not a single giant buffer. */
    int16_t buf[4096 * 2];
    double phase = 0.0, step = 2.0 * M_PI * 440.0 / rate;
    int frames_total = rate;   /* 1 second */
    int written = 0;
    while (written < frames_total) {
        int n = frames_total - written;
        if (n > 4096) n = 4096;
        for (int i = 0; i < n; i++) {
            int16_t s = (int16_t)(sin(phase) * 8000);
            buf[i * 2] = s; buf[i * 2 + 1] = s;
            phase += step;
        }
        snd_pcm_sframes_t w = x_writei(pcm, buf, (snd_pcm_uframes_t)n);
        if (w < 0) { fprintf(stderr, "writei failed rc=%ld\n", (long)w); break; }
        written += (int)w;
    }
    double t_done = ms_since(&t_start);

    x_close(pcm);
    double t_close = ms_since(&t_start);

    printf("RESULT dlopen_ms=%.2f open_ms=%.2f ready_ms=%.2f play_ms=%.2f close_ms=%.2f rate=%u\n",
          t_dlopen, t_open, t_ready, t_done, t_close, rate);
    return 0;
}
