/* audio.c — MP3 decode to ALSA on a worker thread.
 *
 * Device-specific constraints, established by the audiobook mod's on-device
 * probing and followed here:
 *
 *  - HiBy's libmp3.so (libmpg123) is unusable: its file readers are stubs and
 *    mpg123_read returns garbled PCM for 22050 Hz MPEG-2 files. Decode with
 *    minimp3_ex compiled in instead.
 *  - Seek with MP3D_SEEK_TO_BYTE. MP3D_SEEK_TO_SAMPLE builds a full frame
 *    index on the first seek, which OOMs a device with ~56 MB of RAM.
 *  - Open "plughw:0,0" so ALSA resamples the file's native rate to whatever
 *    discrete rates the hardware accepts.
 *  - snd_pcm_set_params is unreliable here; set hw and sw params by hand.
 *
 * libasound is dlopen'd rather than linked so this .so keeps loading even if
 * the library moves.
 */

#define MINIMP3_IMPLEMENTATION
#include "vendor/minimp3_ex.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio.h"

/* ALSA constants, needed because we dlopen rather than include the headers. */
#define SND_PCM_STREAM_PLAYBACK      0
#define SND_PCM_ACCESS_RW_INTERLEAVED 3
#define SND_PCM_FORMAT_S16_LE        2

typedef long snd_pcm_sframes_t;
typedef unsigned long snd_pcm_uframes_t;

static void *g_alsa;
static int   (*x_open)(void **, const char *, int, int);
static int   (*x_close)(void *);
static int   (*x_prepare)(void *);
static int   (*x_drop)(void *);
static snd_pcm_sframes_t (*x_writei)(void *, const void *, snd_pcm_uframes_t);
static int   (*x_recover)(void *, int, int);
static int   (*x_hwp_malloc)(void **);
static void  (*x_hwp_free)(void *);
static int   (*x_hwp_any)(void *, void *);
static int   (*x_hwp_set_access)(void *, void *, int);
static int   (*x_hwp_set_format)(void *, void *, int);
static int   (*x_hwp_set_channels)(void *, void *, unsigned int);
static int   (*x_hwp_set_rate_near)(void *, void *, unsigned int *, int *);
static int   (*x_hwp_set_buffer_time_near)(void *, void *, unsigned int *, int *);
static int   (*x_hwp_set_period_time_near)(void *, void *, unsigned int *, int *);
static int   (*x_hwp_apply)(void *, void *);
static int   (*x_swp_malloc)(void **);
static void  (*x_swp_free)(void *);
static int   (*x_swp_current)(void *, void *);
static int   (*x_swp_set_start_threshold)(void *, void *, snd_pcm_uframes_t);
static int   (*x_swp_apply)(void *, void *);

static const char *g_err;

/* Playback state, guarded by g_lock. */
static pthread_t       g_thread;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int   g_running;      /* worker should keep going */
static int   g_active;       /* a file is loaded */
static int   g_paused;
static int   g_seek_req_ms = -1;
static int   g_pos_ms;
static int   g_dur_ms;
static char  g_path[512];
static int   g_start_ms;

#define SYM(p, name) do { \
    *(void **)(&p) = dlsym(g_alsa, name); \
    if (!p) { g_err = "missing " name; return -1; } \
} while (0)

int audio_init(void) {
    if (g_alsa) return 0;
    g_alsa = dlopen("libasound.so.2", RTLD_LAZY);
    if (!g_alsa) g_alsa = dlopen("libasound.so", RTLD_LAZY);
    if (!g_alsa) { g_err = "no libasound"; return -1; }

    SYM(x_open,      "snd_pcm_open");
    SYM(x_close,     "snd_pcm_close");
    SYM(x_prepare,   "snd_pcm_prepare");
    SYM(x_drop,      "snd_pcm_drop");
    SYM(x_writei,    "snd_pcm_writei");
    SYM(x_recover,   "snd_pcm_recover");
    SYM(x_hwp_malloc,   "snd_pcm_hw_params_malloc");
    SYM(x_hwp_free,     "snd_pcm_hw_params_free");
    SYM(x_hwp_any,      "snd_pcm_hw_params_any");
    SYM(x_hwp_set_access,   "snd_pcm_hw_params_set_access");
    SYM(x_hwp_set_format,   "snd_pcm_hw_params_set_format");
    SYM(x_hwp_set_channels, "snd_pcm_hw_params_set_channels");
    SYM(x_hwp_set_rate_near,        "snd_pcm_hw_params_set_rate_near");
    SYM(x_hwp_set_buffer_time_near, "snd_pcm_hw_params_set_buffer_time_near");
    SYM(x_hwp_set_period_time_near, "snd_pcm_hw_params_set_period_time_near");
    SYM(x_hwp_apply,    "snd_pcm_hw_params");
    SYM(x_swp_malloc,   "snd_pcm_sw_params_malloc");
    SYM(x_swp_free,     "snd_pcm_sw_params_free");
    SYM(x_swp_current,  "snd_pcm_sw_params_current");
    SYM(x_swp_set_start_threshold, "snd_pcm_sw_params_set_start_threshold");
    SYM(x_swp_apply,    "snd_pcm_sw_params");
    return 0;
}

static void *pcm_open(unsigned int rate, int channels) {
    void *pcm = NULL;
    const char *names[] = { "plughw:0,0", "default", "hw:0,0" };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (x_open(&pcm, names[i], SND_PCM_STREAM_PLAYBACK, 0) >= 0 && pcm) break;
        pcm = NULL;
    }
    if (!pcm) { g_err = "snd_pcm_open failed"; return NULL; }

    void *hw = NULL;
    if (x_hwp_malloc(&hw) < 0 || !hw) { g_err = "hw_params_malloc"; goto fail; }
    x_hwp_any(pcm, hw);
    x_hwp_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (x_hwp_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE) < 0) {
        g_err = "S16_LE rejected"; x_hwp_free(hw); goto fail;
    }
    x_hwp_set_channels(pcm, hw, (unsigned)channels);
    unsigned int r = rate;
    x_hwp_set_rate_near(pcm, hw, &r, NULL);
    unsigned int buf_us = 500000, per_us = 100000;   /* 500 ms / 100 ms */
    x_hwp_set_buffer_time_near(pcm, hw, &buf_us, NULL);
    x_hwp_set_period_time_near(pcm, hw, &per_us, NULL);
    if (x_hwp_apply(pcm, hw) < 0) { g_err = "hw_params"; x_hwp_free(hw); goto fail; }
    x_hwp_free(hw);

    void *sw = NULL;
    if (x_swp_malloc(&sw) >= 0 && sw) {
        x_swp_current(pcm, sw);
        x_swp_set_start_threshold(pcm, sw, 1);
        x_swp_apply(pcm, sw);
        x_swp_free(sw);
    }
    x_prepare(pcm);
    return pcm;
fail:
    x_close(pcm);
    return NULL;
}

static void *worker(void *unused) {
    (void)unused;

    mp3dec_ex_t dec;
    memset(&dec, 0, sizeof(dec));
    /* SEEK_TO_BYTE: the sample-accurate mode indexes the whole file and OOMs. */
    if (mp3dec_ex_open(&dec, g_path, MP3D_SEEK_TO_BYTE) != 0) {
        pthread_mutex_lock(&g_lock);
        g_err = "cannot decode file"; g_active = 0; g_running = 0;
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }

    int rate = dec.info.hz ? dec.info.hz : 44100;
    int ch   = dec.info.channels ? dec.info.channels : 2;

    pthread_mutex_lock(&g_lock);
    g_dur_ms = (int)((dec.samples / (ch ? ch : 1)) * 1000ULL / (rate ? rate : 44100));
    pthread_mutex_unlock(&g_lock);

    void *pcm = pcm_open((unsigned)rate, ch);
    if (!pcm) {
        mp3dec_ex_close(&dec);
        pthread_mutex_lock(&g_lock);
        g_active = 0; g_running = 0;
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }

    /* MP3D_SEEK_TO_BYTE resyncs to the next frame rather than tracking an
     * absolute sample index, so cur_sample cannot be used as a clock. Keep our
     * own base + decoded-sample count instead. */
    int base_ms = 0;
    uint64_t frames_since = 0;
    if (g_start_ms > 0) {
        uint64_t target = (uint64_t)g_start_ms * rate / 1000 * ch;
        mp3dec_ex_seek(&dec, target);
        base_ms = g_start_ms;
    }

    static mp3d_sample_t buf[MINIMP3_MAX_SAMPLES_PER_FRAME * 8];
    const size_t want = sizeof(buf) / sizeof(buf[0]);

    for (;;) {
        pthread_mutex_lock(&g_lock);
        int run = g_running, paused = g_paused, seek = g_seek_req_ms;
        g_seek_req_ms = -1;
        pthread_mutex_unlock(&g_lock);
        if (!run) break;

        if (seek >= 0) {
            uint64_t target = (uint64_t)seek * rate / 1000 * ch;
            if (target > dec.samples) target = dec.samples;
            mp3dec_ex_seek(&dec, target);
            base_ms = seek;
            frames_since = 0;
        }
        if (paused) { usleep(60000); continue; }

        size_t got = mp3dec_ex_read(&dec, buf, want);
        if (got == 0) break;                       /* end of file */

        snd_pcm_uframes_t frames = got / (ch ? ch : 1);
        const char *p = (const char *)buf;
        while (frames > 0) {
            snd_pcm_sframes_t n = x_writei(pcm, p, frames);
            if (n < 0) {
                if (x_recover(pcm, (int)n, 1) < 0) { frames = 0; break; }
                continue;
            }
            frames -= (snd_pcm_uframes_t)n;
            p += (size_t)n * ch * sizeof(mp3d_sample_t);
        }

        frames_since += got / (ch ? ch : 1);
        pthread_mutex_lock(&g_lock);
        g_pos_ms = base_ms + (int)(frames_since * 1000ULL / (rate ? rate : 44100));
        pthread_mutex_unlock(&g_lock);
    }

    x_drop(pcm);
    x_close(pcm);
    mp3dec_ex_close(&dec);

    pthread_mutex_lock(&g_lock);
    g_active = 0; g_running = 0; g_paused = 0;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

int audio_play(const char *path, int start_ms) {
    audio_stop();
    if (audio_init() != 0) return -1;

    pthread_mutex_lock(&g_lock);
    snprintf(g_path, sizeof(g_path), "%s", path);
    g_start_ms = start_ms > 0 ? start_ms : 0;
    g_pos_ms = g_start_ms;
    g_dur_ms = 0;
    g_paused = 0;
    g_active = 1;
    g_running = 1;
    g_err = NULL;
    pthread_mutex_unlock(&g_lock);

    if (pthread_create(&g_thread, NULL, worker, NULL) != 0) {
        pthread_mutex_lock(&g_lock);
        g_active = 0; g_running = 0; g_err = "thread failed";
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    return 0;
}

void audio_stop(void) {
    pthread_mutex_lock(&g_lock);
    int was = g_running;
    g_running = 0;
    pthread_mutex_unlock(&g_lock);
    if (was) pthread_join(g_thread, NULL);
    pthread_mutex_lock(&g_lock);
    g_active = 0; g_paused = 0; g_pos_ms = 0; g_dur_ms = 0;
    pthread_mutex_unlock(&g_lock);
}

void audio_toggle_pause(void) {
    pthread_mutex_lock(&g_lock);
    if (g_active) g_paused = !g_paused;
    pthread_mutex_unlock(&g_lock);
}

void audio_seek_relative(int delta_ms) {
    pthread_mutex_lock(&g_lock);
    if (g_active) {
        int t = g_pos_ms + delta_ms;
        if (t < 0) t = 0;
        if (g_dur_ms > 0 && t > g_dur_ms) t = g_dur_ms;
        g_seek_req_ms = t;
        g_pos_ms = t;
    }
    pthread_mutex_unlock(&g_lock);
}

int audio_is_active(void)  { pthread_mutex_lock(&g_lock); int v = g_active; pthread_mutex_unlock(&g_lock); return v; }
int audio_is_paused(void)  { pthread_mutex_lock(&g_lock); int v = g_paused; pthread_mutex_unlock(&g_lock); return v; }
int audio_position_ms(void){ pthread_mutex_lock(&g_lock); int v = g_pos_ms; pthread_mutex_unlock(&g_lock); return v; }
int audio_duration_ms(void){ pthread_mutex_lock(&g_lock); int v = g_dur_ms; pthread_mutex_unlock(&g_lock); return v; }
const char *audio_error(void) { return g_err; }
