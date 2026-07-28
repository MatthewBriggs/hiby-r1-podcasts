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
#include <sys/mman.h>
#include <sys/stat.h>

#include "audio.h"
#include "wsola.h"
#include "mp3meta.h"
#include <fcntl.h>
#include <stdarg.h>
#include <time.h>

static void alog(const char *fmt, ...) {
    char b[200];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    int fd = open("/tmp/.podcast_hook.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { write(fd, b, n > (int)sizeof(b) ? (int)sizeof(b) : n); close(fd); }
}

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
static int             g_thread_valid;  /* g_thread names a thread still to be joined */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int   g_running;      /* worker should keep going */
static int   g_active;       /* a file is loaded */
static int   g_paused;
static int   g_seek_req_ms = -1;
static int   g_pos_ms;
static int   g_dur_ms;
static char  g_path[512];
static int   g_start_ms;
static float g_speed = 1.0f;
static int   g_loading;
/* The CS43131's volume registers are not wired on this device — writes to the
 * ALSA mixer report success but read back 0 and do nothing, which is why the
 * stock player applies volume in software. Wired output therefore scales the
 * samples here. Bluetooth is different: bluealsa exposes a real mixer. */
static int   g_sw_vol = 70;      /* 0-100, wired only */
static int   g_using_bt;

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

/* An A2DP sink shows up as a bluealsa PCM ending in /sink. */
static int bt_sink_connected(void) {
    FILE *p = popen("bluealsa-cli list-pcms 2>/dev/null", "r");
    if (!p) return 0;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), p)) {
        if (strstr(line, "a2dp") && strstr(line, "/sink")) { found = 1; break; }
    }
    pclose(p);
    return found;
}

/* A USB DAC enumerates as its own ALSA card, so "plughw:0,0" — the internal
 * CS43131 — would keep playing out of the headphone jack while the stock player
 * correctly follows the USB output. Find the first card that is not card 0 and
 * prefer it, which is the same rule the Bluetooth check above applies. */
static int usb_card(void) {
    FILE *f = fopen("/proc/asound/cards", "r");
    if (!f) return -1;
    char line[256];
    int card = -1;
    while (fgets(line, sizeof(line), f)) {
        int n;
        /* Entries look like " 1 [Audio          ]: USB-Audio - ..." */
        if (sscanf(line, " %d [", &n) == 1 && n > 0 &&
            (strstr(line, "USB") || strstr(line, "usb"))) {
            card = n;
            break;
        }
    }
    fclose(f);
    return card;
}

static void *pcm_open(unsigned int rate, int channels) {
    void *pcm = NULL;
    /* Route to Bluetooth when a sink is connected, otherwise the wired DAC.
     * "bluealsa" is the predefined plug device and auto-selects the most recent
     * sink, converting rate/format as needed. */
    char usb[24];
    const char *wired[] = { "plughw:0,0", "default", "hw:0,0" };
    const char *usbdev[] = { usb, "default" };
    const char *bt[]    = { "bluealsa" };

    int use_bt = bt_sink_connected();
    int ucard  = use_bt ? -1 : usb_card();
    g_using_bt = use_bt;

    const char **names = wired;
    unsigned count = 3;
    if (use_bt) {
        names = bt; count = 1;
    } else if (ucard > 0) {
        snprintf(usb, sizeof(usb), "plughw:%d,0", ucard);
        names = usbdev; count = 2;
    }
    alog("[audio] output: %s\n",
         use_bt ? "bluetooth" : (ucard > 0 ? usb : "wired"));

    for (unsigned i = 0; i < count; i++) {
        int rc = x_open(&pcm, names[i], SND_PCM_STREAM_PLAYBACK, 0);
        if (rc >= 0 && pcm) break;
        alog("[audio] open %s failed rc=%d\n", names[i], rc);
        pcm = NULL;
    }
    /* Fall back to wired rather than going silent if BT will not open. */
    if (!pcm && use_bt) {
        for (unsigned i = 0; i < 3; i++) {
            if (x_open(&pcm, wired[i], SND_PCM_STREAM_PLAYBACK, 0) >= 0 && pcm) {
                alog("[audio] fell back to wired\n");
                break;
            }
            pcm = NULL;
        }
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
    int hwrc = x_hwp_apply(pcm, hw);
    if (hwrc < 0) {
        alog("[audio] hw_params rc=%d (-16 = EBUSY, slave held elsewhere)\n", hwrc);
        g_err = "hw_params"; x_hwp_free(hw); goto fail;
    }
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

    /* mp3dec_ex_open computes duration by scanning every frame — about 5 s for a
     * podcast episode on this device, which stalls playback start. Drive the
     * frame decoder directly instead and take duration from the Xing header (or
     * the CBR bitrate), which needs only the first few hundred bytes. The file
     * is mmap'd so seeking is just moving a pointer. */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_mutex_lock(&g_lock); g_loading = 1; pthread_mutex_unlock(&g_lock);

    int fd = open(g_path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size < 1024) {
        if (fd >= 0) close(fd);
        pthread_mutex_lock(&g_lock);
        g_err = "cannot open file"; g_active = 0; g_running = 0; g_loading = 0;
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    size_t flen = (size_t)st.st_size;
    const uint8_t *fdata = mmap(NULL, flen, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (fdata == MAP_FAILED) {
        pthread_mutex_lock(&g_lock);
        g_err = "cannot map file"; g_active = 0; g_running = 0; g_loading = 0;
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }

    mp3_meta_t meta;
    if (mp3_meta_parse(fdata, flen, &meta) != 0) {
        munmap((void *)fdata, flen);
        pthread_mutex_lock(&g_lock);
        g_err = "not a supported MP3"; g_active = 0; g_running = 0; g_loading = 0;
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }

    int rate = meta.rate ? meta.rate : 44100;
    int ch   = meta.channels ? meta.channels : 2;

    pthread_mutex_lock(&g_lock);
    g_dur_ms = meta.duration_ms;
    pthread_mutex_unlock(&g_lock);

    wsola_init(rate, ch);
    wsola_set_speed(g_speed);

    void *pcm = pcm_open((unsigned)rate, ch);
    if (!pcm) {
        wsola_free();
        munmap((void *)fdata, flen);
        pthread_mutex_lock(&g_lock);
        g_active = 0; g_running = 0; g_loading = 0;
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    pthread_mutex_lock(&g_lock); g_loading = 0; pthread_mutex_unlock(&g_lock);
    alog("[audio] opened in %ld ms (dur %d ms, toc=%d)\n",
         (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000),
         meta.duration_ms, meta.have_toc);

    mp3dec_t mp3d;
    mp3dec_init(&mp3d);

    size_t pos = meta.audio_start;
    int base_ms = 0;
    uint64_t frames_since = 0;
    if (g_start_ms > 0) {
        pos = mp3_resync(fdata, flen, mp3_meta_seek_offset(&meta, flen, g_start_ms));
        base_ms = g_start_ms;
        mp3dec_init(&mp3d);
    }

    static mp3d_sample_t buf[MINIMP3_MAX_SAMPLES_PER_FRAME];
    static mp3d_sample_t obuf[MINIMP3_MAX_SAMPLES_PER_FRAME * 4];
    const size_t ocap = sizeof(obuf) / sizeof(obuf[0]) / 2;

    for (;;) {
        pthread_mutex_lock(&g_lock);
        int run = g_running, paused = g_paused, seek = g_seek_req_ms;
        float want_speed = g_speed;
        g_seek_req_ms = -1;
        pthread_mutex_unlock(&g_lock);
        if (!run) break;
        if (want_speed != wsola_speed()) wsola_set_speed(want_speed);

        if (seek >= 0) {
            pos = mp3_resync(fdata, flen, mp3_meta_seek_offset(&meta, flen, seek));
            mp3dec_init(&mp3d);
            wsola_reset();
            base_ms = seek;
            frames_since = 0;
        }
        if (paused) { usleep(60000); continue; }

        if (pos + 4 >= flen) break;                    /* end of file */

        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&mp3d, fdata + pos, (int)(flen - pos),
                                          buf, &info);
        if (info.frame_bytes <= 0) break;              /* cannot resync */
        pos += (size_t)info.frame_bytes;
        if (samples <= 0) continue;                    /* skipped/ID3 padding */

        size_t in_frames = (size_t)samples;            /* per channel */
        wsola_feed(buf, in_frames);

        for (;;) {
            size_t out_frames = wsola_read(obuf, ocap);
            if (out_frames == 0) break;

            if (!g_using_bt) {
                pthread_mutex_lock(&g_lock);
                int vol = g_sw_vol;
                pthread_mutex_unlock(&g_lock);
                if (vol < 100) {
                    /* Rough perceptual curve: squaring keeps low settings usable. */
                    int gain = vol * vol * 256 / 10000;   /* 0..256 */
                    size_t n = out_frames * (size_t)ch;
                    for (size_t i = 0; i < n; i++)
                        obuf[i] = (mp3d_sample_t)((obuf[i] * gain) >> 8);
                }
            }
            snd_pcm_uframes_t frames = out_frames;
            const char *p = (const char *)obuf;
            while (frames > 0) {
                snd_pcm_sframes_t n = x_writei(pcm, p, frames);
                if (n < 0) {
                    if (x_recover(pcm, (int)n, 1) < 0) { frames = 0; break; }
                    continue;
                }
                frames -= (snd_pcm_uframes_t)n;
                p += (size_t)n * ch * sizeof(mp3d_sample_t);
            }
            if (out_frames < ocap) break;
        }

        frames_since += in_frames;
        pthread_mutex_lock(&g_lock);
        g_pos_ms = base_ms + (int)(frames_since * 1000ULL / (rate ? rate : 44100));
        pthread_mutex_unlock(&g_lock);
    }

    x_drop(pcm);
    x_close(pcm);
    wsola_free();
    munmap((void *)fdata, flen);

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
    g_loading = 1;
    g_err = NULL;
    pthread_mutex_unlock(&g_lock);

    if (pthread_create(&g_thread, NULL, worker, NULL) != 0) {
        pthread_mutex_lock(&g_lock);
        g_active = 0; g_running = 0; g_err = "thread failed";
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    pthread_mutex_lock(&g_lock);
    g_thread_valid = 1;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void audio_stop(void) {
    /* Join on "a thread exists", not on "it is still running". The worker
     * clears g_running itself when it reaches the end of a file or bails out
     * early, so keying the join off that flag skipped it exactly when the
     * thread had finished on its own — and audio_play then overwrote g_thread,
     * losing the id for good. An exited-but-unjoined thread keeps its stack
     * reservation until the process ends, and this process is hiby_player,
     * which stays up for as long as the device does and runs everything else
     * on it. */
    pthread_mutex_lock(&g_lock);
    g_running = 0;
    int join = g_thread_valid;
    g_thread_valid = 0;
    pthread_mutex_unlock(&g_lock);
    if (join) pthread_join(g_thread, NULL);
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

int audio_is_loading(void) { pthread_mutex_lock(&g_lock); int v = g_loading; pthread_mutex_unlock(&g_lock); return v; }
int audio_is_active(void)  { pthread_mutex_lock(&g_lock); int v = g_active; pthread_mutex_unlock(&g_lock); return v; }
int audio_is_paused(void)  { pthread_mutex_lock(&g_lock); int v = g_paused; pthread_mutex_unlock(&g_lock); return v; }
int audio_position_ms(void){ pthread_mutex_lock(&g_lock); int v = g_pos_ms; pthread_mutex_unlock(&g_lock); return v; }
int audio_duration_ms(void){ pthread_mutex_lock(&g_lock); int v = g_dur_ms; pthread_mutex_unlock(&g_lock); return v; }
const char *audio_error(void) { return g_err; }

int  audio_using_bt(void) { pthread_mutex_lock(&g_lock); int v = g_using_bt; pthread_mutex_unlock(&g_lock); return v; }
int  audio_volume(void)   { pthread_mutex_lock(&g_lock); int v = g_sw_vol;   pthread_mutex_unlock(&g_lock); return v; }
void audio_set_volume(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    pthread_mutex_lock(&g_lock);
    g_sw_vol = pct;
    pthread_mutex_unlock(&g_lock);
}

float audio_speed(void) { pthread_mutex_lock(&g_lock); float v = g_speed; pthread_mutex_unlock(&g_lock); return v; }

void audio_cycle_speed(void) {
    static const float steps[] = { 1.0f, 1.25f, 1.5f, 1.75f, 2.0f };
    pthread_mutex_lock(&g_lock);
    int i = 0;
    for (int k = 0; k < 5; k++) if (steps[k] == g_speed) { i = k; break; }
    g_speed = steps[(i + 1) % 5];
    pthread_mutex_unlock(&g_lock);
}
