/* music_hook.c — a music and radio app on the R1's Stream media launcher tile.
 *
 * Same in-process approach as the Podcasts app: LD_PRELOAD into hiby_player,
 * re-point a launcher tile's callback at us, and take the framebuffer and input
 * for as long as the app is open. The two hooks coexist — they claim different
 * tiles and different code caves.
 *
 *   tile     stream_media, record 0x008925E8, callback slot 0x00892630
 *   cave     0x00760800, a zeroed run in .rodata well clear of the Podcasts
 *            app's 0x0075E400
 *
 * Both addresses were read straight out of hiby_player rather than found by
 * scanning a live device: tile records hold their name as an inline string at
 * +0x00 and the callback at +0x48, so a new firmware can be re-derived in
 * seconds. See the tile-table notes in the repo.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <sys/mount.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <sys/vfs.h>
#include <linux/fb.h>
#include <linux/input.h>

#include "text.h"
#include "icons.h"
#include "library.h"
#include "audiobook.h"
#include "podcast.h"
#include "audio.h"
#include "eq.h"
#include "eqprofile.h"
#include "mseb.h"
#include "recent.h"
#include "cover.h"
#include "art.h"
#include "status.h"
#include "radio.h"
#include "playlist.h"

/* ---- device geometry ----------------------------------------------------- */
#define FB_W 480
#define FB_H 800

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define COL_BG      RGB(16, 16, 20)
#define COL_HEADER  RGB(26, 26, 33)
#define COL_TEXT    RGB(240, 240, 245)
#define COL_DIM     RGB(125, 125, 136)
/* A variable behind this name, not a literal -- R20 wants the accent
 * colour user-selectable, and every draw call already says COL_ACCENT. */
static uint16_t g_accent = RGB(240, 138, 42);
#define COL_ACCENT  g_accent
#define COL_ROW     RGB(26, 26, 33)
#define COL_LINE    RGB(38, 38, 46)

/* R20: a small preset palette rather than a full colour picker -- matching
 * the hardware's own colour variants (or close to them) is the actual ask,
 * not arbitrary RGB entry. */
static const struct { const char *name; uint16_t color; } ACCENT_PRESETS[] = {
    { "Orange", RGB(240, 138, 42)  },
    { "Gold",   RGB(212, 175, 55)  },
    { "Silver", RGB(180, 186, 194) },
    { "Red",    RGB(224, 70, 70)   },
    { "Blue",   RGB(70, 140, 224)  },
    { "Green",  RGB(90, 180, 100)  },
    { "Purple", RGB(160, 100, 220) },
};
#define ACCENT_N ((int)(sizeof(ACCENT_PRESETS) / sizeof(ACCENT_PRESETS[0])))
static int g_accent_idx;
static int button_lock_enabled;   /* off by default: a new gesture, opt in */

#define TEXT_PX_TITLE 34
#define TEXT_PX_BODY  30
#define TEXT_PX_SMALL 22

/* Was 62 -- widened so the header actions (MSEB's Reset, Podcasts' Sync)
 * have a taller strip to land a tap in, not just a wider one. */
#define HEADER_H  78
/* Height of the status strip. Deliberately not STATUS_H-the-include-guard:
 * status.h used that name to guard itself, so whichever came second lost —
 * either this constant was redefined, or the header's contents were skipped
 * entirely. */
#define STATUS_H  32
#define CONTENT_Y (STATUS_H + HEADER_H)
#define ROW_H     72

/* ---- tile hook ----------------------------------------------------------- */
#define TILE_CB       0x00892630u   /* stream_media record + 0x48 */
#define TILE_CB_ORIG  0x0053C300u   /* what it holds on a stock 2.0.26 */
#define CAVE_ADDR     0x00760800u
#define PODCAST_HOOK_PATH "/usr/data/libpodcast_hook.so.real"
#define CAVE_PAGE     (CAVE_ADDR & ~0xFFFu)
#define DATA_PAGE     (TILE_CB & ~0xFFFu)
#define PAGE_SPAN     0x2000u

/* Was /tmp, which a reboot wipes — and a lockup is always followed by a
 * reboot, so the one log that mattered was never there afterwards. */
#define LOG_PATH "/usr/data/music.log"

static uint32_t orig_cb;

/* Set at each step of the index gesture, read by the watchdog. If the loop
 * stalls, the last phase named here is where it stalled. */
static volatile const char *g_phase = "idle";
static volatile unsigned    g_tick;

static void mlog(const char *fmt, ...) {
    char b[256];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    /* Timestamped, like the audio log: without one there is no telling a
     * normal run of short tracks from the player racing through them. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    char line[300];
    int m = snprintf(line, sizeof(line), "[%6ld.%03ld] %s",
                     (long)ts.tv_sec, ts.tv_nsec / 1000000L, b);
    if (m <= 0) return;
    if (m > (int)sizeof(line)) m = (int)sizeof(line);
    int fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { write(fd, line, (size_t)m); close(fd); }
}

/* ---- drawing ------------------------------------------------------------- */
/* ---- artwork ------------------------------------------------------------- */
/* Loaded off the UI thread: a first-time extract-and-decode is a few hundred
 * milliseconds, and the transport must stay live throughout. The UI draws a
 * plain panel until the bitmap appears. */
#define ART_PX FB_W   /* Now Playing art runs edge-to-edge, like stock */

static pthread_mutex_t art_lock = PTHREAD_MUTEX_INITIALIZER;
static uint16_t *art_bits;            /* ART_PX * ART_PX, or NULL */
static char      art_want[512];       /* track the loader should be showing */
/* Bluetooth codec and battery are read on their own thread. They were read
 * from the drawing path, and both shell out — bluealsa-cli and dbus-send. A
 * D-Bus call that blocks (a headset dropping mid-query will do it) then takes
 * the whole UI with it, which looks exactly like the player locking up. The
 * drawing now only ever reads these two strings. */
static pthread_mutex_t bt_lock = PTHREAD_MUTEX_INITIALIZER;
static char bt_codec_cached[32];
static int  bt_batt_cached = -1;
static int  bt_poll_run;
static pthread_t bt_thread;
static int  bt_thread_valid;

/* BG38 (part 2): fuzzy-match a saved EQ profile to whichever headphone is
 * connected, so Sonys getting the Sonys' profile and Jabras getting the
 * Jabras' doesn't need a manual pick every session.
 *
 * The actual eq_switch_to()/save_conf() call has to happen on the main
 * thread: eq_cur/eq_cur_path and everything save_conf() reads are only
 * ever touched from there today, with no lock protecting them, the same
 * as every other UI-owned global in this file. Doing the switch directly
 * from this background thread would be a new, real data race against
 * whatever the main thread is doing with the EQ screen at the same
 * moment. So this thread only ever does the (cheap, stateless) fuzzy
 * match and hands the winning path across through bt_lock -- the same
 * handoff shape already used for the pending Bluetooth volume delta --
 * and the main loop drains it and performs the switch itself. */
static char bt_matched_name[64];      /* device this thread last matched */
static char bt_eq_pending_path[EP_PATH_LEN];   /* set here, applied on the main thread */

/* Case-insensitive, either-direction substring match against each profile's
 * own name (from its filename): forgiving of a device advertising a
 * slightly different string than the profile was saved under ("Sony
 * WH-1000XM4" vs "WH-1000XM4"), which is the whole reason this is fuzzy
 * rather than exact. A minimum length on the shorter side keeps a short or
 * generic profile name (a stray "EQ" or "1") from matching almost anything
 * by accident. First match wins; ep_scan()'s own directory order decides
 * ties, same as everywhere else profiles are listed. Runs on the bt_poll
 * thread, so a local array only -- see the comment above for why it must
 * not touch the shared eq_profiles[]. */
static void bt_match_profile(const char *dev_name) {
    ep_entry_t profiles[EP_MAX_PROFILES];
    int n = ep_scan(profiles, EP_MAX_PROFILES);
    for (int i = 0; i < n; i++) {
        size_t dn = strlen(dev_name), pn = strlen(profiles[i].name);
        if ((dn < 4 && dn <= pn) || (pn < 4 && pn <= dn)) continue;
        if (strcasestr(dev_name, profiles[i].name) ||
            strcasestr(profiles[i].name, dev_name)) {
            pthread_mutex_lock(&bt_lock);
            snprintf(bt_eq_pending_path, sizeof(bt_eq_pending_path), "%s",
                    profiles[i].path);
            pthread_mutex_unlock(&bt_lock);
            return;
        }
    }
}

static void *bt_poll(void *arg) {
    (void)arg;
    while (bt_poll_run) {
        int on_bt = audio_using_bt();
        if (on_bt) {
            char c[32];
            st_bt_codec(c, sizeof(c));
            int b = st_bt_battery();
            audio_bt_volume_service();
            pthread_mutex_lock(&bt_lock);
            snprintf(bt_codec_cached, sizeof(bt_codec_cached), "%s", c);
            bt_batt_cached = b;
            pthread_mutex_unlock(&bt_lock);
        } else {
            pthread_mutex_lock(&bt_lock);
            bt_codec_cached[0] = '\0';
            bt_batt_cached = -1;
            pthread_mutex_unlock(&bt_lock);
        }
        /* BG38 (part 2): connection-based, not audio_using_bt()'s
         * playback-based -- reconnecting a headset with nothing actively
         * routing through it yet (paused, or between tracks) must still
         * get its profile matched rather than wait for playback to touch
         * Bluetooth again, which is the bug a real test caught. st_bt_name()
         * already answers "is something connected" on its own (bt_pcm_path()
         * looks for a live sink PCM in bluealsa's registry -- the same check
         * bt_sink_connected() in audio.c makes for routing), with its own
         * internal 10s cache, so running it every cycle regardless of on_bt
         * is cheap. */
        char nm[64];
        st_bt_name(nm, sizeof(nm));
        if (nm[0]) {
            if (strcmp(nm, bt_matched_name) != 0) {
                bt_match_profile(nm);
                snprintf(bt_matched_name, sizeof(bt_matched_name), "%s", nm);
            }
        } else {
            bt_matched_name[0] = '\0';
        }
        /* Checking for a pending volume write is a cheap mutex read, not a
         * D-Bus round-trip, so it can happen on every sub-tick without
         * cost — the volume key would otherwise wait up to the full ~5s
         * cycle to actually reach the mixer.
         *
         * That reasoning only holds when there is a Bluetooth mixer to write
         * to. Off Bluetooth there is no pending volume this thread can
         * service and nothing it can learn by looking, so the fine-grained
         * sub-tick was ten wakeups a second, indefinitely, to discover
         * nothing -- and this thread lives for the whole life of the process,
         * including every hour the device spends in a pocket. Same ~5s
         * cycle either way; only the granularity changes. */
        int sub_us = on_bt ? 100000 : 1000000;
        int subs   = on_bt ? 50 : 5;
        for (int i = 0; i < subs && bt_poll_run; i++) {
            usleep(sub_us);
            if (on_bt && audio_bt_volume_pending()) audio_bt_volume_service();
        }
    }
    return NULL;
}

static pthread_t art_thread;
static int       art_thread_valid;
static int       art_seq_v;      /* bumped when the bitmap changes */

static int art_seq(void) {
    pthread_mutex_lock(&art_lock);
    int v = art_seq_v;
    pthread_mutex_unlock(&art_lock);
    return v;
}

static void *art_worker(void *arg) {
    (void)arg;
    char track[512];
    pthread_mutex_lock(&art_lock);
    snprintf(track, sizeof(track), "%s", art_want);
    pthread_mutex_unlock(&art_lock);

    char jpg[512], key[512];
    uint16_t *bits = NULL;
    /* Walk the candidates until one actually decodes, rather than trusting the
     * first file that exists. cover_load() returning NULL is a normal outcome,
     * not an error: a large progressive cover or an over-size image is declined
     * on purpose to keep the player out of the OOM killer, and a PNG someone
     * named .jpg simply fails. Any of those used to mean a blank panel even
     * when the track had good embedded art a few candidates further down. */
    for (int n = 0; !bits; n++) {
        int rc = art_candidate(track, n, jpg, sizeof(jpg), key, sizeof(key));
        if (rc == -1) break;
        if (rc == ART_SKIP) continue;
        bits = cover_load(jpg, key, ART_PX);
    }

    pthread_mutex_lock(&art_lock);
    /* Discard if the user has already moved on to another track. */
    if (strcmp(track, art_want) != 0) { free(bits); }
    else { free(art_bits); art_bits = bits; art_seq_v++; }
    pthread_mutex_unlock(&art_lock);
    return NULL;
}

/* Defined with the rest of the Now Playing layout, further down. */
static void title_reset(void);

static void art_request(const char *track) {
    /* New track, new title: a scroll position carried over from the last one
     * would leave the new name starting halfway along. Called from every path
     * that changes what is playing, chapters included. */
    title_reset();
    if (art_thread_valid) { pthread_join(art_thread, NULL); art_thread_valid = 0; }
    pthread_mutex_lock(&art_lock);
    free(art_bits);
    art_bits = NULL;
    art_seq_v++;
    snprintf(art_want, sizeof(art_want), "%s", track);
    pthread_mutex_unlock(&art_lock);
    if (pthread_create(&art_thread, NULL, art_worker, NULL) == 0)
        art_thread_valid = 1;
}

static void blit_art(uint16_t *fb, int x, int y) {
    pthread_mutex_lock(&art_lock);
    if (art_bits) {
        for (int r = 0; r < ART_PX; r++)
            memcpy(fb + (size_t)(y + r) * FB_W + x,
                   art_bits + (size_t)r * ART_PX,
                   (size_t)ART_PX * sizeof(uint16_t));
    }
    pthread_mutex_unlock(&art_lock);
}

/* Nearest-neighbour downsample of the same ART_PX*ART_PX bitmap, for the
 * mini player's thumbnail -- there's no reason to decode/cache a second,
 * smaller copy just for a 56px square when the full one is already resident. */
static void blit_art_scaled(uint16_t *fb, int x, int y, int size) {
    pthread_mutex_lock(&art_lock);
    if (art_bits) {
        for (int r = 0; r < size; r++) {
            const uint16_t *src = art_bits + (size_t)(r * ART_PX / size) * ART_PX;
            uint16_t *dst = fb + (size_t)(y + r) * FB_W + x;
            for (int c = 0; c < size; c++)
                dst[c] = src[c * ART_PX / size];
        }
    }
    pthread_mutex_unlock(&art_lock);
}

static void fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > FB_W) w = FB_W - x;
    if (y + h > FB_H) h = FB_H - y;
    if (w <= 0 || h <= 0) return;
    for (int r = 0; r < h; r++) {
        uint16_t *p = fb + (size_t)(y + r) * FB_W + x;
        for (int i = 0; i < w; i++) p[i] = c;
    }
}

/* The transport was first drawn with text glyphs — "|<", "||", ">|" — which
 * read as a placeholder because the font's bar and bracket do not line up or
 * match weight. These are scanline-filled shapes instead, so the buttons are
 * actually symmetric. */
static void fill_circle(uint16_t *fb, int cx, int cy, int r, uint16_t c) {
    for (int dy = -r; dy <= r; dy++) {
        int yy = cy + dy;
        if (yy < 0 || yy >= FB_H) continue;
        int dx = (int)(sqrt((double)(r * r - dy * dy)) + 0.5);
        fill_rect(fb, cx - dx, yy, dx * 2, 1, c);
    }
}

/* A track or a toggle's background, capped with a half circle at each end
 * instead of a square corner -- the stadium/pill shape sliders and switches
 * actually use. h is expected even; the caps are h/2 each. */
static void fill_pill(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    int r = h / 2;
    if (w <= h) { fill_circle(fb, x + w / 2, y + r, r, c); return; }
    fill_rect(fb, x + r, y, w - 2 * r, h, c);
    fill_circle(fb, x + r, y + r, r, c);
    fill_circle(fb, x + w - r, y + r, r, c);
}

/* A rect with a small corner radius, distinct from fill_pill's full-height
 * stadium caps -- a squat shape like the battery glyph wants a subtle round,
 * not two semicircles eating most of its width. */
static void fill_round_rect(uint16_t *fb, int x, int y, int w, int h, int r, uint16_t c) {
    if (r > h / 2) r = h / 2;
    if (2 * r > w) r = w / 2;
    if (r <= 0) { fill_rect(fb, x, y, w, h, c); return; }
    fill_rect(fb, x, y + r, w, h - 2 * r, c);        /* full-width middle band */
    for (int i = 0; i < r; i++) {
        int dx = (int)(sqrt((double)(r * r - (r - i) * (r - i))) + 0.5);
        int inset = r - dx;
        fill_rect(fb, x + inset, y + i, w - 2 * inset, 1, c);           /* top */
        fill_rect(fb, x + inset, y + h - 1 - i, w - 2 * inset, 1, c);   /* bottom */
    }
}

/* Bresenham, for the glyphs that are all diagonals. */
static void draw_line(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t c) {
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int err = dx - dy;
    for (;;) {
        fill_rect(fb, x0, y0, 2, 2, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

static void draw_toggle_switch(uint16_t *fb, int y, int on) {
    int w = 68, h = 32, x = FB_W - 24 - w;
    fill_pill(fb, x, y + 16, w, h, on ? COL_ACCENT : COL_LINE);
    fill_circle(fb, on ? x + w - h / 2 : x + h / 2, y + 16 + h / 2, h / 2 - 3,
                on ? COL_BG : COL_DIM);
}

/* dir = +1 points right, -1 points left. */
static void fill_triangle(uint16_t *fb, int cx, int cy, int h, int dir, uint16_t c) {
    int half = h / 2;
    int w = (h * 87) / 100;                 /* equilateral-ish, not a wedge */
    for (int dy = -half; dy <= half; dy++) {
        int span = w - (w * (dy < 0 ? -dy : dy)) / half;
        if (span <= 0) continue;
        int x = (dir > 0) ? cx - w / 2 : cx + w / 2 - span;
        fill_rect(fb, x, cy + dy, span, 1, c);
    }
}

/* Arbitrary triangle, for the skip-arc arrowhead below -- fill_triangle()
 * above only does the two axis-aligned cases the transport row needed until
 * now. Standard edge-function fill: a point is inside when it is on the
 * same side of all three edges. One fill_rect() per pixel is wasteful, but
 * this draws an arrowhead a few hundred pixels across, at most a few times a
 * frame. */
static void fill_tri3(uint16_t *fb, int x0, int y0, int x1, int y1,
                      int x2, int y2, uint16_t c) {
    int minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            int w0 = (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0);
            int w1 = (x2 - x1) * (y - y1) - (y2 - y1) * (x - x1);
            int w2 = (x0 - x2) * (y - y2) - (y0 - y2) * (x - x2);
            if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0))
                fill_rect(fb, x, y, 1, 1, c);
        }
    }
}

/* The "skip ten seconds" glyph the audiobook mockup used: a ring left open
 * near the top, with an arrowhead where it breaks -- forward sweeps
 * clockwise from just before 12 o'clock, rewind counter-clockwise from just
 * after it, mirror images of each other. Screen space is y-down, so
 * increasing angle already sweeps visually clockwise; forward is therefore
 * the increasing-angle case and rewind the decreasing one, with no sign
 * flip needed beyond that. */
static void draw_skip_arc(uint16_t *fb, int cx, int cy, int r, int forward, uint16_t c) {
    const double gap = 0.38;                  /* radians of open space at the break */
    const double span = 2.0 * M_PI - 2.0 * gap;
    double a0 = -M_PI / 2 + (forward ? gap : -gap);
    double dir = forward ? 1.0 : -1.0;
    /* Stamped as overlapping dots along the arc, not a true stroke -- cheap,
     * but fill_circle's integer rounding makes a radius-2 dot's outermost row
     * vanish entirely (sqrt(4-4)+0.5 rounds to 0 width), so at the old r=2/
     * steps=40 the dots' usable extent could fall short of their ~3.6px
     * spacing and leave a visible notch at this screen's resolution. r=3
     * keeps a solid band down to a single vanishing row instead of three, and
     * steps=64 shrinks the spacing to ~2.6px -- comfortably inside that
     * band's overlap even at the dot's worst (most-vertical) orientation. */
    int steps = 64;
    for (int i = 0; i <= steps; i++) {
        double a = a0 + dir * span * i / steps;
        int px = cx + (int)(r * cos(a) + 0.5);
        int py = cy + (int)(r * sin(a) + 0.5);
        fill_circle(fb, px, py, 3, c);
    }
    /* Arrowhead at the traveling end, its long axis along the path's
     * tangent so it reads as continuing the sweep rather than just sitting
     * on the ring. */
    double aEnd = a0 + dir * span;
    double tx = -sin(aEnd) * dir, ty = cos(aEnd) * dir;   /* unit tangent */
    double nx = -ty, ny = tx;                             /* unit normal */
    int hx = cx + (int)(r * cos(aEnd) + 0.5);
    int hy = cy + (int)(r * sin(aEnd) + 0.5);
    int ax = hx + (int)(9 * tx), ay = hy + (int)(9 * ty);
    int bx = hx - (int)(3 * tx) + (int)(6 * nx), by = hy - (int)(3 * ty) + (int)(6 * ny);
    int cx2 = hx - (int)(3 * tx) - (int)(6 * nx), cy2 = hy - (int)(3 * ty) - (int)(6 * ny);
    fill_tri3(fb, ax, ay, bx, by, cx2, cy2, c);
}

static void draw_text(uint16_t *fb, int x, int y, const char *s,
                      uint16_t c, int px, int right_edge) {
    text_draw(fb, FB_W, FB_H, 0, x, y, s, c, px, right_edge ? right_edge : FB_W);
}

/* For a row that may be scrolled partway off the top or bottom of a list —
 * clipped pixel-row by pixel-row rather than skipped whole, so a row half
 * revealed by a drag shows its visible half instead of flickering in and out
 * at the row boundary. */
static void draw_text_clip(uint16_t *fb, int x, int y, const char *s,
                           uint16_t c, int px, int right_edge,
                           int clip_top, int clip_bot) {
    text_draw(fb, FB_W, clip_bot, clip_top, x, y, s, c, px, right_edge ? right_edge : FB_W);
}

/* lib_format_name()'s codes come from the stock scanner's own numeric
 * `format` column, read off real device data one value at a time as new
 * formats turned up. Checked a real AIFF file's row directly (BG25): the
 * scanner gives it format=1, the exact code already mapped to "WAV" -- it
 * is not misdetecting the file, it just has one code for "linear PCM"
 * that does not distinguish the RIFF/WAVE container from AIFF's. So an
 * unrecognized code is not the only way this needs a fix: code 1 also needs
 * disambiguating by the one thing the DB row doesn't carry, the container
 * the bytes are actually in. The path's extension answers that directly, the
 * same idea as the audiobook screen already uses for chapters, which have no
 * DB row to read a code from at all. */
static const char *track_format_name(const lib_track_t *t) {
    const char *name = lib_format_name(t->format);
    const char *dot = strrchr(t->path, '.');
    if (!strcmp(name, "WAV") && dot &&
        (!strcasecmp(dot, ".aif") || !strcasecmp(dot, ".aiff")))
        return "AIFF";
    if (strcmp(name, "?") != 0) return name;
    static char ext[16];
    snprintf(ext, sizeof(ext), "%s", dot && dot[1] ? dot + 1 : "?");
    for (char *p = ext; *p; p++) *p = (char)toupper((unsigned char)*p);
    return ext;
}

static void fill_rect_clip(uint16_t *fb, int x, int y, int w, int h, uint16_t c,
                           int clip_top, int clip_bot) {
    if (y < clip_top)  { h -= clip_top - y; y = clip_top; }
    if (y + h > clip_bot) h = clip_bot - y;
    if (h <= 0) return;
    fill_rect(fb, x, y, w, h, c);
}

/* ---- screens ------------------------------------------------------------- */
/* Rows are fetched a screenful at a time rather than held in one array: the
 * library is 4722 tracks and this device has 56 MB. Only what is on screen,
 * plus a little either side, is ever in memory. */
#define PAGE_MAX 32

typedef enum { SC_MENU = 0, SC_MUSIC_MENU, SC_ARTISTS, SC_ALBUMS, SC_TRACKS, SC_PLAYING,
               SC_RADIO, SC_PLAYLISTS, SC_AUDIOBOOKS, SC_PODCASTS, SC_POD_SYNC,
               SC_EQ, SC_EQ_BANDS, SC_EQ_BAND, SC_MSEB,
               SC_SETTINGS, SC_SETTINGS_THEME, SC_SETTINGS_ABOUT } screen_t;

/* L2: the top-level menu ("Main Menu", EXIT on the right) stays small on
 * purpose rather than listing every library-browsing facet alongside
 * Audiobooks/EQ/Radio/Settings -- those facets live one level down, behind
 * "Music" (see menu[] below). */
static const struct { const char *label; } top_menu[] = {
    { "Music" },
    { "Audiobooks" },
    { "Podcasts" },
    { "Parametric EQ" },
    { "MSEB" },
    { "Radio" },
    { "Settings" },
};
#define TOP_MUSIC      0
#define TOP_AUDIOBOOKS 1
#define TOP_PODCASTS   2
#define TOP_EQ         3
#define TOP_MSEB       4
#define TOP_RADIO      5
#define TOP_SETTINGS   6
#define TOP_N ((int)(sizeof(top_menu) / sizeof(top_menu[0])))

/* Reached via "Music" from the main menu (SC_MUSIC_MENU). Each entry is a
 * column to group by, except Albums which skips the grouping step and
 * lists the lot, and Playlists which is not a library query at all. */
static const struct { const char *label; const char *column; } menu[] = {
    { "Album artists",       "album_artist" },
    { "Albums",              NULL           },
    { "Recently added",      NULL           },
    { "Recently heard",      NULL           },
    { "Artists",             "artist"       },
    { "Genres",              "genre"        },
    { "Playlists",           NULL           },
};
#define MENU_RECENT_ADDED 2
#define MENU_RECENT_HEARD 3
#define MENU_PLAYLISTS    6
#define RECENT_ALBUMS_N 10   /* R30: "the last 10 albums", not PAGE_MAX's 32 */
#define MENU_N ((int)(sizeof(menu) / sizeof(menu[0])))

static const char *cur_facet;      /* column being grouped by, NULL for Albums */
static const char *cur_facet_label;
/* 0 normal, 1 Recently added, 2 Recently heard -- set only by those two menu
 * entries, cleared by every other facet pick. See load_page()/
 * index_visible()/the SC_ALBUMS header title for what it changes. R30:
 * SC_ALBUMS's screen and row-tap-to-tracks code are reused as-is for both
 * rather than a new screen, since a recent-albums list is, structurally,
 * just an album list with an unusual sort and no A-Z index -- everything
 * rows[]/row_at()/play_from_list() already do for a normal album row
 * applies unchanged. */
static int         recent_mode;
#define RECENT_ADDED 1
#define RECENT_HEARD 2

/* Held-track actions, drawn over whatever list is showing rather than as a
 * screen of their own: it is a decision about one row, and pushing a screen
 * would lose the reader's place in a long list. */
static int live_x, live_y;          /* where the finger is now, while it is down */

/* Dragging along the progress bar. The seek itself already worked — a drag
 * ends as a tap at the release point — but with nothing moving under your
 * finger there was no way to aim, which is the whole point of scrubbing. */
/* Quick settings, pulled down from the status strip. Brightness, Wi-Fi and
 * Bluetooth are wanted often enough that leaving the app to reach them is the
 * annoyance; everything else stays in the firmware's own settings. */
#define QS_H       490          /* was 418; +QS_ROW_H for the MSEB row */
#define QS_ROW_H   72
/* Row label column. Was a bare 68 until the Wi-Fi/Bluetooth/EQ row icons grew
 * larger -- Wi-Fi's natural width at its new height puts its right edge
 * exactly at 68, no gap at all, so the label column moved out to clear it.
 * +10 past that on top, on request, for a bit more breathing room still. */
#define QS_LABEL_X 86
#define QS_PULL    40           /* how far down before it counts as a pull */
/* BG16: the start zone used to be STATUS_H (32px), and logging real presses
 * showed every miss landing at down_y 33-67 against every hit at 25-31 --
 * the status row itself is just too thin a target to reliably start a
 * swipe from. Widened well past the worst observed miss, but capped where
 * the volume HUD's own drag zone begins (STATUS_H..STATUS_H+VOL_H) so the
 * two don't fight over the same touch when the HUD is up. */
#define QS_PULL_ZONE 80

static int qs_open;
static int qs_bright, qs_bright_max = 101, qs_wifi, qs_bt;
static int qs_dragging;         /* on the brightness bar */

static void qs_refresh(void) {
    qs_bright = st_brightness();
    qs_bright_max = st_brightness_max();
    qs_wifi = st_wifi_on();
    qs_bt = st_bt_on();
}

/* Input devices, rescanned so a headset connecting mid-session is picked up. */
#define KFD_MAX 8
static int  kfd[KFD_MAX];
static int  kfd_src[KFD_MAX];         /* KEYS_BUTTONS or KEYS_REMOTE */
static char kfd_name[KFD_MAX][32];
static int  kfd_n;

/* Volume overlay. Pressing a volume key throws it up; it can also be dragged,
 * which matters because the Bluetooth mixer is a 0-127 scale and stepping it
 * in percent lands between its notches — hence readings like 13%, 19%, 26%. */
#define VOL_H       96
#define VOL_TICKS   90        /* ~3 s at the loop's cadence */
static int vol_ticks, vol_dragging;
static int vol_drag_pct = -1, vol_applied = -1, vol_apply_tick;

static int scrub_active;
/* Running a finger down the A-Z strip. Like scrubbing, this has to be tracked
 * while the finger is down: waiting for the release gives one jump at the end
 * instead of the list moving under your thumb. */
static int index_active;
static int index_shown = -1;        /* letter slot last jumped to */

/* Where the bar sits, needed by both the drawing and the hit test.
 *
 * These used to carry a `title_extra` term, because a title that wrapped to a
 * second line pushed everything below it down and the bar's position was
 * therefore whatever the last frame happened to draw. Titles are one line now
 * (see draw_scroll_title), so the layout below them is fixed again and the
 * hit tests can be read straight off these constants. */
static int title_y(void) { return ART_PX + 20; }

/* Growing the art to edge-to-edge (ART_PX == FB_W, up from 384) ate more
 * vertical space than removing the status strip freed -- 96px added against
 * only 48px freed, which is what made a second title line unaffordable.
 *
 * BG40: this used to be its own formula (title_y() + 104), fourteen pixels
 * above what the main draw path actually put the bar at (title_y() + 118) --
 * both the tap zone here and draw_scrub_strip()'s lightweight redraw during
 * an active drag were quietly eleven-then-three pixels off from where a
 * non-scrubbing frame draws it, which is a real bug (a bar that jumps on the
 * first scrub touch, tap zones biased high), just masked by generous
 * tolerance bands. Now the one shared value everyone -- the main draw path
 * included -- calls, so it cannot drift again. */
static int bar_y(void) { return title_y() + 118; }

/* Same layout formula as bar_y(), two bars instead of one: book (display-only,
 * thin) above chapter (scrubbable, the same weight and hit-test band the
 * regular player's one bar already has). Shared by drawing and the tap test
 * below, exactly as bar_y() is shared for the regular player. */
/* +20 on both, keeping the 40px between them, because the "Book" caption did
 * not clear the line above it. The captions hang 26px above their own bar, so
 * "Book" sat at 560 while the album line at 544 is TEXT_PX_BODY (30) tall and
 * runs to 574 -- straight through it. Only "Book" was affected: "Chapter" at
 * 600 already cleared the book bar at 586. Both move together because shifting
 * the book bar alone would push "Chapter" into it instead, trading one
 * collision for another. At +20 the transport centre lands at 704 and its
 * 42px circle ends at 746, still clear of the footer row at 766. */
static int ab_book_bar_y(void)    { return ART_PX + 20 + 106; }
static int ab_chapter_bar_y(void) { return ART_PX + 20 + 146; }

/* Long titles scroll sideways under the finger instead of wrapping.
 *
 * title_off is what is committed between drags; title_off_live is what the
 * current frame draws, which differs only while a finger is actually on the
 * title. Both are <= 0: 0 is the start of the string, and dragging left goes
 * negative. title_span is how far there is to go, published by the draw code
 * because only it knows the rendered width of the string it just measured. */
static int title_off, title_off_live, title_span, title_dragging;

static void title_reset(void) { title_off = title_off_live = title_span = 0; }

/* SC_EQ_BAND's layout, shared between drawing and the tap/drag hit tests
 * below -- one source of numbers so the two cannot drift apart. */
static int eq_enabled_y(void) { return CONTENT_Y + 20; }
static int eq_type_y(void)    { return eq_enabled_y() + 56 + 26; }
static int eq_freq_y(void)    { return eq_type_y() + 34 + 30 + 24; }
static int eq_gain_y(void)    { return eq_freq_y() + 46; }
static int eq_q_y(void)       { return eq_gain_y() + 46; }

/* SC_EQ overview's rows, same reason. */
static int eq_row_enabled_y(void) { return CONTENT_Y; }
static int eq_row_profile_y(void) { return eq_row_enabled_y() + ROW_H; }
static int eq_preamp_y(void)      { return eq_row_profile_y() + ROW_H; }
static int eq_curve_y(void)       { return eq_preamp_y() + 60; }
static int eq_row_bands_y(void)   { return eq_curve_y() + 66; }

#define MSEB_ROW_H 70
static int mseb_row_enabled_y(void) { return CONTENT_Y; }
static int mseb_band_row_y(int i) { return mseb_row_enabled_y() + ROW_H + i * MSEB_ROW_H; }

/* Left edge of "BACK" itself -- shared by both header actions below so their
 * tap zones can run right up to where BACK's own zone actually starts,
 * rather than guessing at a fixed offset that drifts out of sync with the
 * text the moment either label changes. */
static int header_back_x(void) {
    return FB_W - 24 - text_width("BACK", TEXT_PX_SMALL);
}

/* Left edge of the "Reset" header action, shared between the header's own
 * draw and its tap zone so they cannot drift apart -- the same reason
 * bar_y()/eq_row_*_y() above are functions rather than repeated literals.
 * Positioned left of "BACK" with a 20px gap, both right-aligned inward from
 * the usual 24px margin. */
static int mseb_reset_x(void) {
    int reset_w = text_width("Reset", TEXT_PX_SMALL);
    return header_back_x() - 20 - reset_w;
}

/* Same idea as mseb_reset_x(), for SC_PODCASTS's "Sync" header action. */
static int pod_sync_x(void) {
    int sync_w = text_width("Sync", TEXT_PX_SMALL);
    return header_back_x() - 20 - sync_w;
}

/* BG47 (revised): just two skip arcs now, -10s left of play/pause and +30s
 * right of it -- not the symmetric +/-10/+/-30 four-button set this
 * started as. 96, not the original 70: matches the audiobook screen's own
 * skip-arc offset exactly, per explicit request to space these out the
 * same way that screen does. Offset shared between the draw code and the
 * tap handler, same reason bar_y() is a function rather than a repeated
 * literal. */
#define POD_SKIP_OFF 96
/* Centre x of the podcast speed ring -- mid - 96 - 70, exactly the
 * audiobook screen's own scx formula (see its comment: "96 must match the
 * skip rings' offset above"), not just the same gap. */
static int pod_speed_x(int mid) { return mid - POD_SKIP_OFF - 70; }
/* Centre x of the show-notes info icon -- mirrors pod_speed_x() on the
 * other side of the transport row, same gap past the +30s arc that the
 * speed ring leaves before the -10s one. Moved here from the top-right
 * corner of the cover art per explicit request. */
static int pod_info_x(int mid) { return mid + POD_SKIP_OFF + 70; }

/* How long the device may sit locked with nothing playing before it goes into
 * its low-power idle. Minutes, and 0 means never. Declared up here with the
 * layout rather than down with the rest of the config, because the settings
 * screen draws the current value well before load_conf() is defined.
 *
 * What "low-power idle" means deliberately stops short of suspend-to-RAM for
 * now: writing `mem` to /sys/power/state on this device hangs it partway
 * through the sequence -- the panel blanks, the backlight stays lit, and
 * nothing brings it back, not the power button and not an RTC alarm armed and
 * verified beforehand. The stock player reaches suspend through its own
 * teardown first (it ships a separate /usr/bin/bt_suspend, has an early-suspend
 * phase, and carries the string "fb has already suspended"), and until that
 * order is worked out, a setting that can wedge the device is not one worth
 * shipping. What this timeout drives instead is releasing the parts that
 * actually draw current -- see deep_suspend(). */
static const int SLEEP_CHOICES[] = { 0, 1, 2, 5, 10, 30 };
#define SLEEP_CHOICE_N ((int)(sizeof(SLEEP_CHOICES) / sizeof(SLEEP_CHOICES[0])))
static int sleep_idx = 2;                       /* index into SLEEP_CHOICES */
static int sleep_minutes(void) { return SLEEP_CHOICES[sleep_idx]; }

/* Whether that same timeout also suspends the SoC (see deep_suspend()).
 * Declared here with the rest of the persisted settings because load_conf()
 * reads it long before the suspend code itself is defined. */
static int deep_sleep_enabled;

/* A full poweroff after sitting locked with nothing playing, independent of
 * the suspend timer above and not sharing its gate. This is the direct
 * answer to what suspend turned out to cost: an unattended overnight test
 * with deep sleep on needed a hard power cycle and still measured no real
 * improvement, and the working reference implementation's own suspend
 * sequence (recovered from open_hiby_player's binary) puts the SDIO bus that
 * Wi-Fi and the SD card share through a full teardown and rebuild on every
 * cycle -- fine once, risky dozens of times unattended, and that
 * investigation is paused pending its source. A power-off draws whatever the
 * PMIC's own standby leakage is, which is not a figure this codebase can
 * improve on, and costs a real cold boot to get back rather than a
 * suspend's sub-second wake -- a trade explicitly chosen over suspend's
 * fragility for a "left alone for hours" scenario. Off by default, same as
 * deep sleep: a feature that ends the process is not one to default on
 * before it has been run for real. */
static const int AUTO_OFF_CHOICES[] = { 0, 5, 15, 30, 60, 120 };
#define AUTO_OFF_CHOICE_N ((int)(sizeof(AUTO_OFF_CHOICES) / sizeof(AUTO_OFF_CHOICES[0])))
static int auto_off_idx;                        /* index into AUTO_OFF_CHOICES */
static int auto_off_minutes(void) { return AUTO_OFF_CHOICES[auto_off_idx]; }

/* SC_SETTINGS's rows, same reason. */
/* The +40 below each description used to be the gap before the *next row's
 * title*, back when the divider sat above the description instead of below
 * it. It was never big enough to hold the description itself: text_draw's y
 * is the top of the line box, glyphs draw from y+ascent, and a descender on
 * the second line ("again", "playing") reaches past dy+40 -- so the divider,
 * now placed right after the description, cut through those descenders.
 * +64 gives the two-line block genuine clearance rather than reusing a
 * number sized for a different layout. */
static int set_row_lock_y(void)  { return CONTENT_Y; }
static int set_lock_desc_y(void) { return set_row_lock_y() + ROW_H; }
/* "Idle sleep" used to sit here. Its row is gone from Settings: it drove
 * deep_suspend(), and that whole line of work is paused until there is source
 * for open_hiby_player to compare against -- an unattended overnight run with
 * it on ended in a hard power cycle and measured no improvement. The timeout
 * is still read from music.conf so an existing config still parses, but with
 * deep_sleep defaulting to 0 nothing reaches the suspend path, and there is no
 * longer a way to switch it on by accident from the UI. Auto shutdown is the
 * shipped answer to the same problem. */
static int set_row_autooff_y(void)  { return set_lock_desc_y() + 64; }
static int set_autooff_desc_y(void) { return set_row_autooff_y() + ROW_H; }
static int set_row_theme_y(void) { return set_autooff_desc_y() + 64; }
/* R26: About, one row below Accent colour -- which now needs its own
 * trailing divider back (it used to be the last row and closed the list
 * itself), and this row takes over closing the list instead. */
static int set_row_about_y(void) { return set_row_theme_y() + ROW_H; }

/* Bump this with every release -- it had been stuck at "0.1" since the very
 * first one, through 0.14, because nothing ever reminded anyone to touch it.
 * There is no build-time derivation from the git tag (this .so is built and
 * pushed by hand, not by CI against a tagged commit), so this stays a
 * literal that a human edits; the discipline is remembering to, not the
 * mechanism. */
#define LIBRARY_VERSION "0.16"

static void about_kernel(char *out, size_t n) {
    struct utsname u;
    if (uname(&u) == 0) snprintf(out, n, "%s", u.release);
    else snprintf(out, n, "unknown");
}

/* config.json's own "version" key appears twice -- a bare integer for the
 * slef entry ahead of it ("version":1, no quotes) and a quoted string for
 * the product entry that actually matters here ("version":"1.6a"). The
 * quote right after the colon is what tells them apart, confirmed against
 * the file directly rather than assumed. patch_firmware.py stamps this
 * field itself (stamp_config_json()), so it already reads a
 * build-identifying value like "1.6a", not the vendor's plain "1.6". */
static void about_firmware(char *out, size_t n) {
    snprintf(out, n, "unknown");
    FILE *f = fopen("/usr/resource/config.json", "r");
    if (!f) return;
    char buf[4096];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[got] = '\0';
    const char *p = strstr(buf, "\"version\":\"");
    if (!p) return;
    p += strlen("\"version\":\"");
    const char *end = strchr(p, '"');
    if (!end) return;
    size_t len = (size_t)(end - p);
    if (len >= n) len = n - 1;
    memcpy(out, p, len);
    out[len] = '\0';
}

/* R26 follow-up. The efuse chip ID is the right source (confirmed live --
 * hiby_player's own strings name both this /proc path and getSerialNumber
 * together), but the raw 32-hex-digit ID is not itself the serial number:
 * the stock About screen's "Serial number" is "R1" + the first 8 hex digits
 * of the chip ID, uppercased. Confirmed by direct comparison on this unit --
 * chip ID "7b42423542a0c500afa0560804000001" against stock's own displayed
 * "R17B424235". First attempt showed the full raw ID; wrong, per the user. */
static void about_serial(char *out, size_t n) {
    snprintf(out, n, "unknown");
    FILE *f = fopen("/proc/jz/efuse/efuse_chip_id", "r");
    if (!f) return;
    char buf[128];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return; }
    fclose(f);
    const char *p = strstr(buf, "CHIP_ID:");
    if (p) p += strlen("CHIP_ID:"); else p = buf;
    while (*p == ' ') p++;
    if (strlen(p) < 8) return;
    char hex8[9];
    memcpy(hex8, p, 8);
    hex8[8] = '\0';
    for (int i = 0; i < 8; i++) hex8[i] = (char)toupper((unsigned char)hex8[i]);
    snprintf(out, n, "R1%s", hex8);
}

/* SD_ROOT matches library.c's own -- /data is a symlink to usr/data, so this
 * resolves to the same mount statfs sees at /usr/data/mnt/sd_0 either way. */
static void about_sd_free(char *out, size_t n) {
    struct statfs sf;
    if (statfs("/data/mnt/sd_0/", &sf) != 0 || sf.f_bsize <= 0) {
        snprintf(out, n, "unknown");
        return;
    }
    double gb = (double)sf.f_bavail * (double)sf.f_bsize / (1000.0 * 1000.0 * 1000.0);
    snprintf(out, n, "%.1f GB free", gb);
}

/* MemAvailable (not MemFree): what the kernel itself estimates could actually
 * be given to a new allocation without swapping, once reclaimable cache is
 * counted -- MemFree alone reads alarmingly low on this device even when
 * plenty is available, for exactly the reason BG44's own investigation ran
 * into (buff/cache regularly the largest share of a 56 MB budget). */
static void about_ram_free(char *out, size_t n) {
    snprintf(out, n, "unknown");
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        long kb;
        if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1) {
            snprintf(out, n, "%.0f MB free", kb / 1024.0);
            break;
        }
    }
    fclose(f);
}

/* One line, dragged sideways rather than wrapped.
 *
 * This replaced a split_title() that broke a long name at its first colon or
 * " - " and drew two lines. That was a good fit while the artwork was 384px
 * and there was room underneath; with the cover running edge to edge there is
 * not, and a wrapped title pushed the transport controls into the footer.
 * Dragging also beats splitting on its own terms for this library: it shows
 * the *whole* string, including the tail a split still had to ellipse, and it
 * does not have to guess where the interesting half of a classical title
 * starts.
 *
 * Publishes title_span so the drag handler knows how far there is to go --
 * only the draw side knows the rendered width of the string. */
static void draw_scroll_title(uint16_t *fb, int y, const char *s) {
    int avail = FB_W - 48;
    int w = text_width(s, TEXT_PX_TITLE);
    title_span = w > avail ? w - avail : 0;
    if (title_off_live < -title_span) title_off_live = -title_span;
    if (title_off < -title_span)      title_off = -title_span;

    draw_text(fb, 24 + title_off_live, y, s, COL_TEXT, TEXT_PX_TITLE, FB_W - 24);
    /* text_draw only clips at the framebuffer edge, so whatever has slid past
     * the left margin is painted back out here. Without this, letters sit in
     * the margin and read as a rendering fault rather than a scroll. */
    fill_rect(fb, 0, y - 2, 24, TEXT_PX_TITLE + 12, COL_BG);
}

/* h:mm:ss past an hour, m:ss below it. Everything an audiobook shows needs
 * this: m:ss alone made an eleven-hour book "663:33" and a two-hour chapter
 * of Blue Mars "119:47". Tracks use it too — a Mahler symphony movement is
 * not far off the same problem. */
static void fmt_dur(char *out, size_t n, int64_t ms) {
    if (ms < 0) ms = 0;
    int s = (int)(ms / 1000);
    if (s >= 3600) snprintf(out, n, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else           snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

static void fmt_left(char *out, size_t n, int64_t ms) {
    char t[32];
    fmt_dur(t, sizeof(t), ms);
    snprintf(out, n, "%s left", t);
}

static int scrub_ms(int dur) {
    int px = live_x - 24, span = FB_W - 48;
    if (px < 0) px = 0;
    if (px > span) px = span;
    return dur > 0 ? (int)((long long)dur * px / span) : 0;
}

static int  sheet_open;                 /* 0 none, 1 actions, 2 pick a playlist */
static int  sheet_track;                /* index into tracks[] */
static char sheet_note[64];             /* what the last action did */
static const char *const sheet_items[] = {
    "Play next", "Add to queue", "Add to playlist", "Cancel"
};
#define SHEET_N ((int)(sizeof(sheet_items) / sizeof(sheet_items[0])))
#define SHEET_ROW 72

static pl_t playlists[PL_MAX];
static int  playlist_n;

static screen_t screen;
static int scroll;                      /* index of the first visible row */
static int scroll_px;                   /* 0..ROW_H-1: how far into that row */
static int total;                       /* rows in the current list */

/* BG37: SC_ARTISTS' own scroll position, saved the moment a row is tapped
 * (whichever facet -- Album artists, Artists, Genres, all share this same
 * screen and tap handler) and restored in go_back()'s SC_ALBUMS case, so
 * returning from an artist's albums lands back where you were rather than
 * at the top. One slot, not a stack: this only ever needs to undo the one
 * level SC_ARTISTS -> SC_ALBUMS goes, the same scope BG7's albums_artist
 * already covers for the artist filter itself. */
static int artists_scroll_saved, artists_scroll_px_saved;

/* BG37 follow-up: the same idea, one level down. SC_ALBUMS' own scroll
 * position was never saved at all -- BG37 only ever covered SC_ARTISTS ->
 * SC_ALBUMS, not SC_ALBUMS -> SC_TRACKS, so backing out of an album always
 * landed on top of the Albums list regardless. Saved the moment an album
 * row is tapped, restored in go_back()'s default case (the only path back
 * from SC_TRACKS to a real SC_ALBUMS browse -- the ab_list branch there is
 * Chapters, a different screen's data, and ALBUMS-with-no-facet is a
 * distinct forward entry that correctly wants the top). */
static int albums_scroll_saved, albums_scroll_px_saved;

/* Live drag + inertia state for list scrolling, set up alongside scroll_to_px
 * below (which needs vis_rows/load_page, not yet declared here). */
static int   list_dragging;
static int   list_down_y, list_start_px;
static int   list_last_y;
static unsigned list_last_tick;
static float  list_velocity;            /* px per tick, signed */
static int    inertia_active;

static int index_lock_end = -1; /* absolute row index the last index jump can't show past, -1 = none */

static lib_row_t rows[PAGE_MAX];
static int row_n;
static int row_base = -1;       /* absolute offset of rows[0], -1 when stale */
static void index_cache_reset(void);

static void reset_scroll(void) {
    scroll = 0; scroll_px = 0;
    list_dragging = 0; inertia_active = 0; list_velocity = 0;
    index_lock_end = -1;
    row_base = -1;
    row_n = 0;
    index_cache_reset();
}
static lib_track_t tracks[PAGE_MAX * 8];
static int track_n;

/* What is playing has to survive browsing away from it: open another album and
 * tracks[] is replaced, so a queue that was just an index into it would start
 * naming the wrong song in the mini player. The queue is therefore its own
 * copy, with the artist and album it came from. */
static radio_station_t stations[RADIO_MAX];
static int  station_n;
/* A stream has no queue, no length and no artwork, so Now Playing has to know
 * it is showing one rather than a track. */
static int  radio_mode;
static char radio_name[LIB_NAME_LEN];
static char radio_msg[64];        /* why the last station did not start */

/* Audiobooks. Chapters reuse tracks[]/queue[] exactly as an album's tracks
 * do -- ab_scan_chapters() fills lib_track_t the same shape sweep_album_
 * folder() does -- so play_from_list(), the mini player, hardware keys and
 * the queue-icon-into-SC_TRACKS mechanism all work unchanged. Only the Now
 * Playing screen itself needs new drawing and input handling, gated on this
 * flag exactly as SC_PLAYING already gates on radio_mode. */
/* Two separate facts, and conflating them meant browsing an album while a
 * book played drew the album with the audiobook's transport. `audiobook_mode`
 * is what is *playing*, and is set and cleared beside radio_mode wherever
 * playback starts; `ab_list` is whether tracks[] currently holds a book's
 * chapters, which is about where the browser is. */
static int  audiobook_mode;
static int  ab_list;
static ab_book_t ab_books[AB_MAX_BOOKS];
static lib_row_t ab_rows[AB_MAX_BOOKS];   /* ab_books[], reshaped for the row-list draw code */
static int  ab_book_n;
static int  ab_speed_permille = 1000;     /* persists across books; reset only by leaving the mode */
/* The open book. Chapters are also mirrored into tracks[]/queue[] so the
 * chapter list, the mini player and Now Playing's title all keep working
 * through the same code an album's tracks use; this carries the part a
 * lib_track_t has nowhere to put — where in its file each chapter starts. */
static ab_book_data_t ab_book;
static char ab_playing[LIB_PATH_LEN];     /* the file the decoder actually has open */

static int was_active;    /* to spot a track ending of its own accord */

/* Podcasts. Feeds are a flat list, drawn and tapped exactly like
 * SC_AUDIOBOOKS (pod_rows[]/row_at()); an episode list of one feed reuses
 * SC_TRACKS exactly like a book's chapters do (`pod_list` alongside
 * `ab_list`). Playback is deliberately NOT queued across a whole feed the
 * way an album's tracks are -- queue_n stays 1, one episode at a time --
 * since a feed mixes downloaded and not-yet-downloaded episodes and a
 * podcast queue isn't something a listener expects to play straight
 * through the way an album is. `podcast_mode` is what's *playing*, set and
 * cleared beside audiobook_mode/radio_mode; `pod_list` is whether tracks[]
 * currently holds one feed's episodes, same split as ab_list/audiobook_mode. */
#define POD_MAX_FEEDS 64
static pod_feed_t pod_feeds[POD_MAX_FEEDS];
static lib_row_t  pod_rows[POD_MAX_FEEDS];   /* pod_feeds[], reshaped for the row-list draw code */
static int        pod_feed_n;
static pod_episode_t pod_eps[POD_MAX_ITEMS]; /* parallel to tracks[] while pod_list is set */
static int        pod_ep_n;
static char       cur_feed[POD_NAME_LEN];
static int        podcast_mode;
static int        pod_list;
/* BG47: separate from ab_speed_permille on purpose -- persists across
 * episodes the same way ab_speed_permille persists across books, but a
 * podcast's chosen speed has no reason to leak into or be overwritten by
 * an audiobook's. */
static int        pod_speed_permille = 1000;
/* Show notes: raw text, wrapped and clipped at draw time. Loaded once, when
 * an episode starts, not re-read every frame. */
static char       pod_notes_text[8192];
static int        pod_notes_avail;
static int        pod_notes_showing;
static int        pod_notes_scroll_px;
static int        pod_notes_dragging;
static int        pod_notes_down_y, pod_notes_start_px;

/* SC_POD_SYNC: a dedicated screen for the whole-feed sync, per BACKLOG.md's
 * L5 entry ("the UPDATE FEEDS screen, which streams live podsync subprocess
 * output into its own screen") -- not folded into a toast or a status line
 * on SC_PODCASTS, since a multi-feed sync can run long enough that the
 * reader wants to see it's actually making progress, not just that it
 * started. Filled from pod_update_tail() every tick while running (see the
 * main loop), newest last, same as the fetcher's own log file reads. */
#define POD_SYNC_LOG_N 10
static char pod_sync_log[POD_SYNC_LOG_N][POD_NAME_LEN];
static int  pod_sync_log_n;

/* Parametric EQ. eq.c/eq.h own the live filter state applied in the audio
 * thread; this is the UI-side working copy, written to eq_set_band()/
 * eq_set_preamp() at the same call sites that update it locally, so the two
 * never need an explicit resync. eq_cur_path is empty when nothing has been
 * loaded yet -- the EQ then sits at its all-zero (silent no-op) default. */
static ep_entry_t  eq_profiles[EP_MAX_PROFILES];
static int         eq_profile_n;
static char        eq_cur_path[EP_PATH_LEN];
static eq_profile_t eq_cur;
static int         eq_editing_band;       /* index into eq_cur.band[] on SC_EQ_BAND */
/* Which slider is being dragged live, on whichever of SC_EQ/SC_EQ_BAND is
 * showing -- 0 none, 1 preamp, 2 frequency, 3 gain, 4 Q. Mirrors qs_dragging/
 * vol_dragging: touch-down-in-band starts it, touch-up ends it, live_x drives
 * the value every tick in between. */
static int         eq_dragging;

static float       mseb_gain[MSEB_BAND_N];
static int         mseb_on;
/* -1 none, else the band index being dragged on SC_MSEB. Same live_x-driven
 * pattern as eq_dragging, kept separate since MSEB has 9 independent
 * sliders rather than eq_dragging's fixed handful of named ones. */
static int         mseb_dragging = -1;

static lib_track_t queue[PAGE_MAX * 8];
static int  queue_n;
static char q_artist[LIB_NAME_LEN];
static char q_album[LIB_NAME_LEN];
static char cur_artist[LIB_NAME_LEN];
static char cur_album[LIB_NAME_LEN];
/* What cur_artist was filtering the Albums list by, before a tap into a
 * specific album's Tracks overwrote it with that one album's artist (BG7):
 * two artists can share an album name, so the track query needs the exact
 * artist, but that leaves nothing telling "back" whether Albums was ever
 * filtered at all. Set wherever SC_ALBUMS is actually entered as a browse
 * target, restored into cur_artist on the way back out of Tracks. */
static char albums_artist[LIB_NAME_LEN];
/* Set right before a normal Albums -> Tracks -> Playing descent; cleared at
 * the mini-player's direct jump into Playing. Tells go_back()'s SC_PLAYING
 * case whether albums_artist (the "artist" facet column's group name, e.g.
 * "Alice Sara Ott") is still good, or whether q_artist (BG7's fallback,
 * which is really the album_artist *column*'s value, e.g. "Alice Sara Ott
 * (piano)" -- a different tag that can hold a different string) is the only
 * thing available. Without this, backing out of a normal browse clobbered a
 * perfectly good albums_artist with a value from the wrong column, and the
 * next "back" filtered Albums by a facet value that matched nothing. */
static int played_from_browse;
static int  cur_track;          /* index into tracks[] */

/* An A-Z strip down the right edge. 117 artists and 277 albums nine rows at a
 * time is not navigable by swiping, and this is a great deal cheaper to build
 * and to use than a keyboard on a 480x800 screen. */
#define INDEX_W 45          /* half again as wide: it is a thumb, not a stylus */
/* The strip is drawn INDEX_W wide but caught wider: a thumb lands short of
 * the edge more often than it lands on it, and the rows have nothing out
 * here to lose, since their text is already clipped inside INDEX_W. */
#define INDEX_TOUCH_W 72
#define INDEX_N 27                    /* # then A-Z */

/* Sliding left off the strip subdivides the letter under the thumb: B is a
 * shelf of Bruckner, Beethoven and Brahms, and one stop for all of them is
 * no stop at all. Only second letters that exist are offered, so every band
 * leads somewhere. INDEX_SUB_DEAD is the slack before the first one engages,
 * so an ordinary vertical run down the strip never trips it. */
#define INDEX_SUB_MAX  12
#define INDEX_SUB_DEAD 70
#define INDEX_PFX_MAX  200

/* Gathered when the index is first grabbed, not while the thumb is moving. */
static char index_pfx[INDEX_PFX_MAX * 2];
static int  index_pfx_n;
static char index_pfx_key[LIB_NAME_LEN + 16];   /* what it was gathered for */
static int  index_letter_off[LIB_INDEX_LETTERS];
static int  index_letter_off_ready;
typedef struct { char key[3]; int off; } index_pair_off_t;
static index_pair_off_t index_pair_off[INDEX_PFX_MAX];
static int index_pair_off_n;

static char index_sub_lock;     /* the letter the strip is held inside, 0 when not */
static char index_sub[INDEX_SUB_MAX];
static int  index_sub_n;      /* subkeys under the current letter */

static int index_visible(void);
static void load_page(void);

static char index_letter(int i) { return i == 0 ? '#' : (char)('A' + i - 1); }

#define MINI_H 76

/* Shared between draw_mini() and its tap handler, rather than the two each
 * carrying their own copy of these numbers -- BG21 this same session was
 * exactly that drift (a hit zone left pointing at a button's old position
 * after only the draw side moved), and duplicating the numbers here would
 * invite the same bug the next time this row's layout changes. Positions
 * are button centres; zones are the x past which the next button to the
 * right takes over, so each one only has to be right of its own left
 * neighbour and left of its own right one -- not aligned to any particular
 * edge of the glyph drawn at it. */
#define MINI_BTN_R     20              /* every button in a shared row, incl. play/pause */
#define MINI_PLAY_CX   (FB_W - 90)     /* play/pause, when it shares the bar */
#define MINI_SIDE_CX   (FB_W - 34)     /* next (music) / skip-forward (audiobook) */
#define MINI_BACK_CX   (FB_W - 146)    /* skip-back, audiobook only */
#define MINI_ZONE_SIDE (FB_W - 62)     /* right of this: the side button */
#define MINI_ZONE_PLAY (FB_W - 118)    /* right of this: play/pause */
#define MINI_ZONE_BACK (FB_W - 174)    /* right of this: skip-back (audiobook) */

/* The mini player sits over the bottom of the list, so the list has to give up
 * the rows it covers or the last one is unreachable. */
static int mini_visible(void);
static int vis_rows(void) {
    int h = FB_H - CONTENT_Y - (mini_visible() ? MINI_H : 40);
    return h / ROW_H;
}

/* Only the two lists long enough to need it -- and not Recent, capped at
 * PAGE_MAX items and sorted by recency rather than alphabetically, where a
 * letter strip would have nothing meaningful to jump to. */
static int index_visible(void) {
    return (screen == SC_ARTISTS || screen == SC_ALBUMS) && !recent_mode;
}

static int index_bottom(void) { return FB_H - (mini_visible() ? MINI_H : 40); }

/* How many stops the strip is showing: 27 letters, or the second letters of
 * the one it is locked to. */
static int index_stops(void) { return index_sub_lock ? index_sub_n : INDEX_N; }

/* The label for stop `i` — "B" at the top level, "Br" inside one. */
static void index_label(int i, char *out) {
    if (index_sub_lock) { out[0] = index_sub_lock; out[1] = index_sub[i]; out[2] = '\0'; }
    else                { out[0] = index_letter(i); out[1] = '\0'; }
}

static void index_cache_reset(void) {
    index_pfx_n = 0;
    index_pfx_key[0] = '\0';
    index_letter_off_ready = 0;
    index_pair_off_n = 0;
}

/* This work belongs to picking up the A-Z strip, not every ordinary touch in
 * an indexed list. The letter offsets are one aggregate query, replacing two
 * count-distinct scans for every letter crossed during a fast drag. */
static void index_prepare(void) {
    char want[LIB_NAME_LEN + 16];
    snprintf(want, sizeof(want), "%d/%s/%s", (int)screen,
             cur_facet ? cur_facet : "", cur_artist);
    if (strcmp(want, index_pfx_key) == 0) return;

    index_pfx_n = lib_prefixes(cur_facet,
                               cur_artist[0] ? cur_artist : NULL,
                               screen == SC_ALBUMS,
                               index_pfx, INDEX_PFX_MAX);
    index_letter_off_ready =
        lib_letter_offsets(cur_facet, cur_artist[0] ? cur_artist : NULL,
                           screen == SC_ALBUMS, index_letter_off) == 0;
    index_pair_off_n = 0;
    snprintf(index_pfx_key, sizeof(index_pfx_key), "%s", want);
    mlog("[music] index: %d prefixes, letter offsets %s for %s\n",
         index_pfx_n, index_letter_off_ready ? "ready" : "unavailable", want);
}

/* Single keys are already covered by the aggregate map. Second-letter keys
 * are much rarer, so memoising their exact legacy query keeps their SQLite
 * comparison behaviour without carrying the whole distinct list in memory. */
static int index_offset(const char *key) {
    if (!key[1] && key[0] >= 'A' && key[0] <= '[' && index_letter_off_ready)
        return index_letter_off[key[0] - 'A'];

    for (int i = 0; i < index_pair_off_n; i++)
        if (!strcmp(key, index_pair_off[i].key)) return index_pair_off[i].off;

    int off = (screen == SC_ARTISTS) ? lib_group_offset(cur_facet, key)
                                     : lib_albums_offset(cur_facet,
                                                         cur_artist[0] ? cur_artist : NULL,
                                                         key);
    if (key[1] && index_pair_off_n < INDEX_PFX_MAX) {
        snprintf(index_pair_off[index_pair_off_n].key,
                 sizeof(index_pair_off[index_pair_off_n].key), "%s", key);
        index_pair_off[index_pair_off_n++].off = off;
    }
    return off;
}

static void draw_index(uint16_t *fb) {
    int top = CONTENT_Y + 4, bot = index_bottom() - 4;
    int n = index_stops();
    int step = (bot - top) / (n > 0 ? n : 1);
    for (int i = 0; i < n; i++) {
        char c[4];
        index_label(i, c);
        int w = text_width(c, TEXT_PX_SMALL);
        int on = index_active && i == index_shown;
        int cy2 = top + i * step + (step - 18) / 2;
        if (on) {
            /* A droplet pointing back at the strip: round where the letter
             * sits, tapering to a point on the right. Far enough out to clear
             * the thumb, and dark-on-orange so it reads at a glance. */
            int r = 22, cx = FB_W - INDEX_W - 110;
            fill_circle(fb, cx, cy2 + 9, r, COL_ACCENT);
            for (int dx = 0; dx <= r + 16; dx++) {
                int half = r - (r * dx) / (r + 16);
                if (half > 0) fill_rect(fb, cx + dx, cy2 + 9 - half, 1, half * 2, COL_ACCENT);
            }
            draw_text(fb, cx - w / 2, cy2, c, COL_BG, TEXT_PX_SMALL, FB_W);
        } else {
            draw_text(fb, FB_W - INDEX_W / 2 - w / 2, cy2, c, COL_DIM, TEXT_PX_SMALL, FB_W);
        }
    }
}

/* Jump so the first entry beginning with the touched letter is at the top. */
static int index_slot(int y) {
    int top = CONTENT_Y + 4, bot = index_bottom() - 4;
    int n = index_stops();
    int step = (bot - top) / (n > 0 ? n : 1);
    if (step <= 0 || n <= 0) return -1;
    int i = (y - top) / step;
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    return i;
}

static void index_jump_key(int i, const char *key) {
    if (i < 0) return;

    int off = 0;
    if (i > 0) off = index_offset(key);
    mlog("[music] jumpkey i=%d key0=%d key1=%d off=%d sub_lock=%d\n",
         i, (int)(unsigned char)key[0], (int)(unsigned char)key[1], off, (int)(unsigned char)index_sub_lock);
    int max_top = total - vis_rows();
    if (max_top < 0) max_top = 0;

    /* The viewport must never show past the end of the top-level letter this
     * jump landed in — whether that's a plain single-letter jump (E has 4
     * albums and room for 8: no amount of dragging should pull in F) or a
     * locked subdivision (Es within E, which may still show sibling
     * subletters like Ex, just never F). Both share one boundary: the start
     * of the next top-level letter, key[0]+1 — true even when locked, since
     * index_label puts the locked letter itself in key[0]. */
    if (i > 0 && key[0]) {
        char next[2] = { (char)(key[0] + 1), '\0' };
        int end = index_offset(next);
        int letter_max = end - vis_rows();
        /* Floored at off, not 0: when the letter is shorter than a screen
         * (most of them, for a library this size), end - vis_rows() falls
         * below where the letter starts, and clamping to 0 dragged the
         * whole jump back toward the start of the alphabet — pulling in the
         * tail of the previous letter above U rather than the next one
         * below it. Anchoring at its own start and letting the remainder of
         * the screen show past the end is the same trade-off already made
         * below; there is no version of this that shows only U when U is
         * shorter than a screen. */
        if (letter_max < off) letter_max = off;
        if (letter_max < max_top) max_top = letter_max;
        index_lock_end = end;
    } else {
        index_lock_end = -1;          /* '#', the very top: nothing to bound */
    }

    if (off > max_top) off = max_top;
    if (off < 0) off = 0;
    scroll = off;
    scroll_px = 0;
    inertia_active = 0; list_velocity = 0;
    load_page();
}

/* The key for a slot, with its second letter when one is selected. */
static void index_key(int i, char *out) { index_label(i, out); }

static void index_jump(int y) {
    char key[3]; int i = index_slot(y);
    if (i < 0) return;
    index_key(i, key);
    /* Slot 0 is '#' only at the top level; inside a letter every stop is real. */
    index_jump_key(index_sub_lock ? 1 : i, key);
}

/* ab_books[] -> ab_rows[], so the existing lib_row_t-shaped draw loop and
 * row_at() can serve the book list without knowing it isn't a database
 * query. A book list is tens to low hundreds of entries, nothing like the
 * 4700-track library the paged rows[]/row_base machinery exists for, so the
 * whole thing is just kept in memory rather than paged. */
static void ab_rebuild_rows(void) {
    for (int i = 0; i < ab_book_n; i++) {
        snprintf(ab_rows[i].name, sizeof(ab_rows[i].name), "%s", ab_books[i].title);
        /* 0 draws nothing. A one-file book was reporting "1" beside a title
         * whose chapter list has seventeen entries in it, which said the
         * opposite of the truth. */
        ab_rows[i].count = ab_books[i].file_count > 1 ? ab_books[i].file_count : 0;
        ab_rows[i].owner[0] = '\0';
    }
}

/* pod_feeds[] -> pod_rows[], same reasoning as ab_rebuild_rows() above. */
static void pod_rebuild_rows(void) {
    for (int i = 0; i < pod_feed_n; i++) {
        snprintf(pod_rows[i].name, sizeof(pod_rows[i].name), "%s", pod_feeds[i].name);
        pod_rows[i].count = 0;
        pod_rows[i].owner[0] = '\0';
    }
}

/* pod_eps[] -> tracks[], same reasoning as ab_load_book() mirroring chapters
 * into tracks[] -- an undownloaded episode gets an empty path, same as any
 * other lib_track_t whose file isn't there yet, so the tap handler is what
 * has to check pod_eps[idx].downloaded, not audio_play() failing at runtime. */
static void pod_rebuild_tracks(void) {
    int max = (int)(sizeof(tracks) / sizeof(tracks[0]));
    track_n = 0;
    for (int i = 0; i < pod_ep_n && track_n < max; i++) {
        lib_track_t *t = &tracks[track_n];
        memset(t, 0, sizeof(*t));
        snprintf(t->name, sizeof(t->name), "%s", pod_eps[i].name);
        if (pod_eps[i].downloaded)
            snprintf(t->path, sizeof(t->path), "%s", pod_eps[i].path);
        t->dur_ms = pod_eps[i].dur_ms;
        t->track = -1;
        t->disc  = -1;
        track_n++;
    }
}

/* Mirrors ab_save_current_pos() exactly -- see its comment for why the
 * audio_is_active() guard matters (audio_stop() zeroes g_pos_ms on the way
 * out, and podcast_mode/cur_track/queue are static, carrying stale values
 * into the next call otherwise). The last few seconds count as finished
 * rather than a resumable position, so re-opening a just-completed episode
 * starts over instead of resuming one second from the end. */
static void pod_save_current_pos(void) {
    if (!podcast_mode || !audio_is_active()) return;
    if (cur_track < 0 || cur_track >= queue_n) return;
    int pos = audio_pos_ms(), dur = audio_dur_ms();
    /* BG48/BG49's root cause reaches here too: audio_dur_ms() is always 0
     * for MP3 (see pod_probe_dur()'s comment in podcast.c), so without this
     * fallback the dur>0 check below could never see a real duration and an
     * episode could never be marked POD_FINISHED, only ever a plain resume
     * position -- same fallback the draw/seek code already uses. */
    if (dur <= 0) dur = queue[cur_track].dur_ms;
    if (dur > 0 && pos >= dur - 3000) pod_resume_store(queue[cur_track].path, POD_FINISHED, dur);
    else pod_resume_store(queue[cur_track].path, pos, dur);
}

/* One episode, playing on its own -- see the comment by podcast_mode for why
 * this doesn't queue the rest of the feed the way play_from_list() does for
 * an album. Resumes from pod_resume_lookup() when there is a saved position;
 * a POD_FINISHED episode restarts from 0, same as reaching the end of any
 * other track leaves nothing to resume into. */
static void pod_play_episode(int idx) {
    if (idx < 0 || idx >= pod_ep_n || !pod_eps[idx].downloaded) return;
    pod_save_current_pos();
    radio_mode = 0;
    audiobook_mode = 0;
    podcast_mode = 1;
    memcpy(&queue[0], &tracks[idx], sizeof(queue[0]));
    queue_n = 1;
    cur_track = 0;
    q_album[0] = '\0';
    q_artist[0] = '\0';
    audio_set_next(NULL);
    audio_set_speed(pod_speed_permille);
    audio_play(pod_eps[idx].path);
    art_request(pod_eps[idx].path);
    was_active = 1;
    int dur = 0;
    int resume = pod_resume_lookup(pod_eps[idx].path, &dur);
    if (resume > 0 && resume != POD_FINISHED) audio_seek_ms(resume);
    pod_notes_avail = pod_load_notes(pod_eps[idx].path, pod_notes_text, sizeof(pod_notes_text)) > 0;
    pod_notes_showing = 0;
    pod_notes_scroll_px = 0;
    mlog("[music] podcast episode %s\n", pod_eps[idx].name);
}

static void load_page(void) {
    if (screen == SC_AUDIOBOOKS) return;   /* ab_rows[] is already complete, no paging */
    if (screen == SC_PODCASTS) return;     /* pod_rows[] likewise -- a folder count, not a query */
    if (!index_visible()) return;

    /* Keep a compact window around the viewport. The rows used to be fetched
     * at `scroll` on every crossed row; a 32-row page only displays about nine
     * at a time, so retaining its overlap avoids most GROUP BY/OFFSET queries
     * while keeping the same row-stepped presentation. */
    int base = scroll - (PAGE_MAX - vis_rows()) / 2;
    int max_base = total - PAGE_MAX;
    if (max_base < 0) max_base = 0;
    if (base < 0) base = 0;
    if (base > max_base) base = max_base;

    if (screen == SC_ARTISTS)
        row_n = lib_group(cur_facet, rows, PAGE_MAX, base);
    else
        row_n = lib_albums(cur_facet, cur_artist, rows, PAGE_MAX, base);
    row_base = base;
}

static int page_covers_viewport(void) {
    if (!index_visible() || row_base < 0) return 0;
    int end = scroll + vis_rows();
    if (end > total) end = total;
    return scroll >= row_base && end <= row_base + row_n;
}

static lib_row_t *row_at(int absolute) {
    if (screen == SC_AUDIOBOOKS)
        return (absolute >= 0 && absolute < ab_book_n) ? &ab_rows[absolute] : NULL;
    if (screen == SC_PODCASTS)
        return (absolute >= 0 && absolute < pod_feed_n) ? &pod_rows[absolute] : NULL;
    int local = absolute - row_base;
    return local >= 0 && local < row_n ? &rows[local] : NULL;
}

/* Scroll to an absolute pixel offset (scroll*ROW_H + scroll_px), clamped to
 * the list's range. Only re-pages the database-backed lists (ARTISTS,
 * ALBUMS), and only when the whole-row part actually changes — dragging
 * across a screenful of rows re-pages a handful of times, not once a pixel,
 * since PAGE_MAX (32) comfortably covers what fits on screen either way. */
/* Returns 1 if the visible rows actually changed. A drag no longer slides
 * rows across the screen a pixel at a time -- they stay exactly where they
 * are and simply hold different content once the finger has moved a full
 * row's worth of distance, the way turning a page does rather than
 * scrolling one. Nothing else needs to change to get this: scroll_px still
 * tracks how far into the next row the drag has gone, for continuity across
 * ticks, but the draw loop only ever sees it at 0, since dirty is now only
 * raised on the tick a row actually crosses. That is the whole fix for the
 * smoothness this was chasing — not a cheaper redraw, just far fewer of
 * them: one per 72px of travel instead of one on nearly every tick. */
static int scroll_to_px(int total_px) {
    int limit = (screen == SC_TRACKS) ? track_n :
                (screen == SC_RADIO)  ? station_n :
                (screen == SC_PLAYLISTS) ? playlist_n : total;
    int max_top = limit - vis_rows();
    if (max_top < 0) max_top = 0;
    int max_px = max_top * ROW_H;
    if (total_px < 0) total_px = 0;
    if (total_px > max_px) total_px = max_px;
    int new_scroll = total_px / ROW_H;
    int new_px     = total_px % ROW_H;
    int changed = new_scroll != scroll;
    if (changed) {
        scroll = new_scroll;
        if (index_visible() && !page_covers_viewport()) load_page();
    }
    scroll_px = new_px;
    return changed;
}

/* Right-aligned secondary figure: album count, track count, duration.
 *
 * Anchored inside the A-Z strip where one is showing. Anchoring to the screen
 * edge put these digits underneath the letters — which is what "the numbers
 * overlap the quick scroll letters" meant, and removing the numbers was the
 * wrong reading of it. */
static void draw_right(uint16_t *fb, int y, const char *s) {
    int w = text_width(s, TEXT_PX_SMALL);
    int right = FB_W - 24 - (index_visible() ? INDEX_W : 0);
    draw_text(fb, right - w, y, s, COL_DIM, TEXT_PX_SMALL, FB_W);
}

static void draw_right_clip(uint16_t *fb, int y, const char *s, int clip_top, int clip_bot) {
    int w = text_width(s, TEXT_PX_SMALL);
    int right = FB_W - 24 - (index_visible() ? INDEX_W : 0);
    draw_text_clip(fb, right - w, y, s, COL_DIM, TEXT_PX_SMALL, FB_W, clip_top, clip_bot);
}

/* Every route into playback goes through here: tapping a track, and the two
 * transport buttons. Keeping it in one place is what stops the artwork request
 * from being dropped on one path and not the others. */
#define QUEUE_MAX ((int)(sizeof(queue) / sizeof(queue[0])))

static void queue_follower(void);

/* Insert a track into the queue. at < 0 appends. If nothing is playing the
 * queue is started from this album first, so "add to queue" from a cold start
 * does something sensible rather than nothing. */
static void queue_insert(int track_idx, int at) {
    if (track_idx < 0 || track_idx >= track_n) return;
    if (queue_n == 0) {
        snprintf(q_artist, sizeof(q_artist), "%s", cur_artist);
        snprintf(q_album,  sizeof(q_album),  "%s", cur_album);
    }
    if (queue_n >= QUEUE_MAX) return;
    if (at < 0 || at > queue_n) at = queue_n;
    memmove(&queue[at + 1], &queue[at],
            sizeof(queue[0]) * (size_t)(queue_n - at));
    queue[at] = tracks[track_idx];
    queue_n++;
    if (at <= cur_track && queue_n > 1) cur_track++;   /* keep pointing at the same song */
    queue_follower();                                  /* what comes next may have changed */
    mlog("[music] queued %s at %d\n", queue[at].name, at);
}

static void play_station(int i) {
    if (i < 0 || i >= station_n) return;
    radio_msg[0] = '\0';
    if (!stations[i].url[0]) {
        snprintf(radio_msg, sizeof(radio_msg), "%s has no URL yet", stations[i].name);
        return;
    }
    if (!st_net_up()) {
        snprintf(radio_msg, sizeof(radio_msg), "Wi-Fi is off");
        return;
    }
    radio_mode = 1;
    audiobook_mode = 0;
    podcast_mode = 0;
    audio_set_speed(1000);           /* a stream has no WSOLA use for it */
    snprintf(radio_name, sizeof(radio_name), "%s", stations[i].name);
    art_request("");                 /* clears whatever art was showing */
    audio_play(stations[i].url);
    was_active = 1;
    mlog("[music] station %s\n", stations[i].name);
}

/* Hand the worker the track after this one so it can roll straight into it. */
static void queue_follower(void) {
    int nxt = cur_track + 1;
    audio_set_next((!radio_mode && nxt < queue_n) ? queue[nxt].path : NULL);
}

static void play_index(int i) {
    radio_mode = 0;
    audiobook_mode = 0;
    podcast_mode = 0;
    if (i < 0 || i >= queue_n) return;
    cur_track = i;
    audio_play(queue[i].path);
    art_request(queue[i].path);
    queue_follower();
    was_active = 1;
    /* R30's "recently heard" side. q_album is the queue's album, set by
     * play_from_list() before this ever runs -- empty here means the queue
     * came from somewhere that never set it (radio, before that guard was
     * added, say), not a real album to record. Marked on every track start,
     * not just the first: an album played track-by-track over an evening
     * should read as heard "now", not at whatever moment side one began. */
    if (q_album[0]) recent_heard_mark(q_album);
    mlog("[music] play %s\n", queue[i].path);
}

/* Starting a track from the browser makes that album the queue. */
static void play_from_list(int idx) {
    if (idx < 0 || idx >= track_n) return;
    memcpy(queue, tracks, sizeof(queue[0]) * (size_t)track_n);
    queue_n = track_n;
    snprintf(q_artist, sizeof(q_artist), "%s", cur_artist);
    snprintf(q_album,  sizeof(q_album),  "%s", cur_album);
    play_index(idx);
}

/* ---- audiobooks ---------------------------------------------------------- *
 * A chapter is a stretch of a file rather than a file, so these three take
 * the place of play_from_list/play_index/the advance handler. Everything
 * else — the chapter list, the mini player, the hardware keys — goes on
 * using tracks[]/queue[], which is why the chapters are mirrored into them. */

static void ab_load_book(const ab_book_t *b) {
    ab_open_book(b->dir, &ab_book);
    int max = (int)(sizeof(tracks) / sizeof(tracks[0]));
    track_n = 0;
    for (int i = 0; i < ab_book.chap_n && track_n < max; i++) {
        lib_track_t *t = &tracks[track_n];
        memset(t, 0, sizeof(*t));
        snprintf(t->name, sizeof(t->name), "%s", ab_book.chap[i].title);
        snprintf(t->path, sizeof(t->path), "%s", ab_book.files[ab_book.chap[i].file]);
        t->dur_ms = (int)ab_book.chap[i].dur_ms;
        t->track = -1;
        t->disc  = -1;
        track_n++;
    }
    snprintf(cur_album, sizeof(cur_album), "%s", b->title);
    cur_artist[0] = '\0';
    ab_list = 1;
}

/* The next file after the one chapter `i` lives in, for gapless roll-over.
 * Chapters inside a single file need nothing queued — the decoder never
 * reaches the end of the file at a chapter boundary. */
static const char *ab_next_file(int i) {
    if (i < 0 || i >= ab_book.chap_n) return NULL;
    int f = ab_book.chap[i].file;
    for (int j = i + 1; j < ab_book.chap_n; j++)
        if (ab_book.chap[j].file != f) return ab_book.files[ab_book.chap[j].file];
    return NULL;
}

static void ab_play_chapter(int i) {
    if (i < 0 || i >= ab_book.chap_n || i >= track_n) return;
    audiobook_mode = 1;
    radio_mode = 0;
    podcast_mode = 0;
    if (queue_n != track_n) {
        memcpy(queue, tracks, sizeof(queue[0]) * (size_t)track_n);
        queue_n = track_n;
        snprintf(q_album, sizeof(q_album), "%s", cur_album);
        q_artist[0] = '\0';
    }
    cur_track = i;
    const char *path = ab_book.files[ab_book.chap[i].file];
    /* Chapters of one file share a decoder: moving between them is a seek,
     * not a reopen, which is what keeps a tap on chapter 12 of an eleven-hour
     * book instant rather than a fresh parse of the whole moov. */
    if (strcmp(ab_playing, path) != 0 || !audio_is_active()) {
        snprintf(ab_playing, sizeof(ab_playing), "%s", path);
        audio_play(path);
        art_request(path);
    }
    audio_seek_ms((int)ab_book.chap[i].file_start_ms);
    audio_set_next(ab_next_file(i));
    was_active = 1;
    mlog("[music] chapter %d/%d %s at %lldms\n", i + 1, ab_book.chap_n,
         ab_book.chap[i].title, (long long)ab_book.chap[i].file_start_ms);
}

/* Write the position playing right now, if any -- called wherever a book
 * might stop being the thing on screen: leaving the player, switching to a
 * different book, and app shutdown. Also called on a timer (see the main
 * loop) so a crash or a battery pull loses at most a few seconds, not the
 * whole session. A no-op whenever there is nothing to save, so callers do
 * not need to guard it themselves. */
static void ab_save_current_pos(void) {
    /* audio_is_active(), not just audiobook_mode: audio_stop() zeroes
     * g_pos_ms on the way out, and audiobook_mode/cur_track/ab_book are
     * static -- they carry the previous session's values into a fresh
     * music_entry() until something new plays. Without this guard, the very
     * first save call of a new session (the book-tap handler's "save
     * whatever was playing before switching") reads that stale zero and
     * overwrites a real saved position with 0 before ab_resume_book() ever
     * gets to read it back. Caught by hand: exited mid-chapter-5, reopened
     * the book, and it silently came back at 0:00 -- this is why. */
    if (!audiobook_mode || !audio_is_active()) return;
    if (cur_track < 0 || cur_track >= ab_book.chap_n) return;
    ab_save_position(&ab_book, ab_book.chap[cur_track].file, audio_pos_ms());
}

/* The chapter a saved (file, ms) position falls inside -- same shape as
 * ab_follow()'s search, but starting cold from a resume rather than from
 * wherever cur_track already was. */
static int ab_chapter_for(int file_idx, long file_ms) {
    int best = 0;
    for (int i = 0; i < ab_book.chap_n; i++)
        if (ab_book.chap[i].file == file_idx && ab_book.chap[i].file_start_ms <= file_ms)
            best = i;
    return best;
}

/* Start the book where it was left, if it ever has been. play_chapter() also
 * covers a book with no saved position -- chapter 0 falls out of the same
 * (file=0, ms=0) shape the "never played" case already returns. */
static void ab_resume_book(void) {
    int fi = 0; long ms = 0;
    ab_load_position(ab_book.dir, &fi, &ms);
    if (fi >= ab_book.file_n) fi = 0;
    int i = ab_chapter_for(fi, ms);
    ab_play_chapter(i);
    /* play_chapter() already seeks to the chapter's own start; a resume
     * partway through that chapter corrects it by the difference. */
    if (ms > ab_book.chap[i].file_start_ms) audio_seek_ms((int)ms);
    mlog("[music] resumed %s at file %d, %ldms\n", ab_book.dir, fi, ms);
}

/* Average bitrate of the file actually on disk -- an AAC file states no
 * nominal rate the way a FLAC header does, so this is size*8/duration, the
 * same arithmetic VBR bitrate has always meant. Cached on the path: a stat()
 * per frame is cheap enough here, but there is no reason to pay it thirty
 * times a second when the file has not changed. */
static char ab_bitrate_path[AB_PATH_LEN];
static int  ab_bitrate_kbps;

static int ab_file_bitrate_kbps(const char *path, int64_t file_dur_ms) {
    if (strcmp(path, ab_bitrate_path) != 0) {
        struct stat st;
        ab_bitrate_kbps = (file_dur_ms > 0 && stat(path, &st) == 0)
                        ? (int)((int64_t)st.st_size * 8 / file_dur_ms) : 0;
        snprintf(ab_bitrate_path, sizeof(ab_bitrate_path), "%s", path);
    }
    return ab_bitrate_kbps;
}

/* Which chapter the playing position is now inside. Derived rather than
 * remembered, so simply listening across a boundary moves the display on
 * without anything having to notice the crossing. */
static int ab_follow(void) {
    if (!audiobook_mode || ab_book.chap_n <= 0) return 0;
    int c = cur_track;
    if (c < 0 || c >= ab_book.chap_n) return 0;
    int64_t pos = audio_pos_ms();
    int f = ab_book.chap[c].file;
    while (c + 1 < ab_book.chap_n && ab_book.chap[c + 1].file == f &&
           pos >= ab_book.chap[c + 1].file_start_ms) c++;
    while (c > 0 && ab_book.chap[c - 1].file == f &&
           pos < ab_book.chap[c].file_start_ms) c--;
    if (c == cur_track) return 0;
    cur_track = c;
    return 1;
}

/* ---- parametric EQ -------------------------------------------------------- */

/* True once eq_cur_path already points at a fork this app made -- nothing
 * left to protect there, so further edits just save it in place. */
static int eq_is_custom_path(const char *path) {
    static const char suf[] = " (Custom).txt";
    size_t n = strlen(path), sn = sizeof(suf) - 1;
    return n >= sn && strcmp(path + n - sn, suf) == 0;
}

/* The first edit to an imported profile forks it to "<name> (Custom).txt"
 * instead of overwriting the import in place -- a file dropped in from
 * AutoEq or a headphone vendor stays exactly what was dropped in. Once on a
 * fork, eq_is_custom_path() is true and this is a no-op; further edits land
 * on the fork directly, the same file, not a fresh "(Custom) (Custom)". */
static void eq_fork_if_needed(void) {
    if (eq_is_custom_path(eq_cur_path)) return;
    char dir[EP_PATH_LEN];
    snprintf(dir, sizeof(dir), "%s", eq_cur_path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0; else dir[0] = 0;
    /* eq_cur.name is both source and destination below -- snprintf reading
     * and writing the same buffer is undefined behaviour, and in practice
     * lost everything but the literal suffix. Read it out first. */
    char base[EP_NAME_LEN];
    snprintf(base, sizeof(base), "%s", eq_cur.name);
    snprintf(eq_cur_path, sizeof(eq_cur_path), "%s/%s (Custom).txt", dir, base);
    snprintf(eq_cur.name, sizeof(eq_cur.name), "%s (Custom)", base);
}

/* Persist a live edit back into the file it came from -- the point of storing
 * profiles as plain text in EQProfiles rather than only in eq.c's memory. A
 * no-op with nothing loaded (a fresh install, before any profile exists).
 * Only ever called from an actual edit site (see the tap handlers below),
 * never defensively on the way out of a screen -- each edit already saves
 * itself the moment it happens, so a save here always means something
 * genuinely changed, which is what makes forking-on-first-edit safe. */
static void eq_save_current(void) {
    if (!eq_cur_path[0]) return;
    eq_fork_if_needed();
    ep_save(eq_cur_path, &eq_cur);
}

static void eq_switch_to(const char *path) {
    ep_load(path, &eq_cur);
    snprintf(eq_cur_path, sizeof(eq_cur_path), "%s", path);
    eq_set_profile(&eq_cur);
}

/* Frequency sliders are log-scaled (20 Hz - 20 kHz): a linear slider spends
 * 99% of its travel above 2 kHz, which is backwards for how parametric bands
 * are actually placed. t is 0..1 across the slider's width. */
static float eq_freq_from_t(float t) {
    if (t < 0) t = 0; if (t > 1) t = 1;
    return 20.0f * powf(1000.0f, t);   /* 20 Hz at t=0, 20000 Hz at t=1 */
}
static float eq_t_from_freq(float hz) {
    if (hz < 20.0f) hz = 20.0f; if (hz > 20000.0f) hz = 20000.0f;
    return logf(hz / 20.0f) / logf(1000.0f);
}

static int mini_visible(void) {
    return audio_is_active() && screen != SC_PLAYING &&
           (queue_n > 0 || radio_mode);
}

/* Cover (or book cover), title, subtitle and a pause button, over the bottom
 * of any list screen. A live stream has no art to show -- art_request("") is
 * what play_station() already clears art_bits with -- so radio keeps the
 * original text-only layout instead of a thumbnail-shaped hole. */
static void draw_mini(uint16_t *fb) {
    int by = FB_H - MINI_H;
    fill_rect(fb, 0, by, FB_W, MINI_H, COL_HEADER);
    fill_rect(fb, 0, by, FB_W, 1, COL_LINE);

    /* R32: a 1px position indicator at the very bottom edge of the screen --
     * not the mini-player's own top border (already drawn above), the last
     * row of the framebuffer itself, corner to corner. Display only, same
     * as the full player's own scrub strip's dur/pos sourcing (draw_scrub_
     * strip) but with no track/background under it and no scrub handling --
     * this is glanceable position while browsing, not a second place to
     * drag. Radio has no meaningful duration, so it draws nothing here
     * rather than a bar that can never move. */
    if (!radio_mode && cur_track >= 0 && cur_track < queue_n) {
        lib_track_t *t = &queue[cur_track];
        int dur = audio_dur_ms();
        if (dur <= 0) dur = t->dur_ms;
        if (dur > 0) {
            int pos = audio_pos_ms();
            int w = FB_W * pos / dur;
            if (w > FB_W) w = FB_W;
            if (w > 0) fill_rect(fb, 0, FB_H - 1, w, 1, COL_ACCENT);
        }
    }

    int thumb = 56, tx = 12, ty = by + (MINI_H - thumb) / 2;
    int text_x = 20;
    if (!radio_mode) {
        /* COL_ROW (the full player's own art placeholder) is the same value
         * as this bar's COL_HEADER background, so it would be invisible
         * here specifically -- COL_LINE instead, for real contrast against
         * the bar while art loads or when a track has none. */
        fill_rect(fb, tx, ty, thumb, thumb, COL_LINE);
        blit_art_scaled(fb, tx, ty, thumb);
        text_x = tx + thumb + 12;
    }

    /* Text clips before whichever control cluster is narrowest for this
     * mode -- radio's lone play button leaves the most room, audiobook's
     * three buttons the least. */
    int text_edge = radio_mode ? FB_W - 104
                  : audiobook_mode ? MINI_ZONE_BACK - 20
                  : MINI_ZONE_PLAY - 20;
    if (radio_mode) {
        draw_text(fb, text_x, by + 12, radio_name, COL_TEXT, TEXT_PX_SMALL, text_edge);
        draw_text(fb, text_x, by + 42, "Internet radio", COL_DIM, TEXT_PX_SMALL, text_edge);
    } else {
        lib_track_t *t = &queue[cur_track];
        draw_text(fb, text_x, by + 12, t->name, COL_TEXT, TEXT_PX_SMALL, text_edge);
        /* A chapter carries no artist -- tracks[] built by ab_load_book()
         * never sets one -- so the regular artist line would be blank here.
         * q_album is the book title, set once when the queue was loaded and
         * stable regardless of what's being browsed elsewhere meanwhile.
         * For music, the track's own artist wins when it has one: q_artist
         * is the *album's* artist (BG30 -- a track's own tag can genuinely
         * differ from it, e.g. a compilation, and showing the album's
         * artist for every track was wrong). q_artist is only a fallback
         * for the rare track with no artist tag of its own at all. */
        const char *sub = audiobook_mode ? q_album
                         : (t->artist[0] ? t->artist : q_artist);
        draw_text(fb, text_x, by + 42, sub, COL_DIM, TEXT_PX_SMALL, text_edge);
    }

    int cy = by + MINI_H / 2;
    /* Radio is the one mode with nothing either side of play/pause, so it
     * keeps the original larger button and position rather than shrinking
     * to match a cluster it isn't part of. */
    int cx = radio_mode ? FB_W - 46 : MINI_PLAY_CX;
    int pr = radio_mode ? 26 : MINI_BTN_R;
    fill_circle(fb, cx, cy, pr, COL_ACCENT);
    if (audio_is_paused()) {
        fill_triangle(fb, cx + 2, cy, pr - 4, +1, COL_BG);
    } else {
        /* Bar size scales with the circle instead of a fixed size left over
         * from when this button was always drawn at the same one radius --
         * the previous formula doubled the height by mistake (an errant
         * "* 2"), which combined with the smaller shared-row radius is what
         * made the bars look oversized. */
        int bw = pr / 4, bh = pr - 4, gap = bw;
        fill_rect(fb, cx - bw - gap / 2, cy - bh / 2, bw, bh, COL_BG);
        fill_rect(fb, cx + gap / 2,      cy - bh / 2, bw, bh, COL_BG);
    }

    if (audiobook_mode) {
        /* Plain white -10/+10, no circle -- play/pause is the only filled
         * button in the row, so it stays the one thing that reads as "the
         * button" at a glance. */
        const char *back_lbl = "-10", *fwd_lbl = "+10";
        draw_text(fb, MINI_BACK_CX - text_width(back_lbl, TEXT_PX_SMALL) / 2,
                  cy - TEXT_PX_SMALL / 2 + 2, back_lbl, COL_TEXT, TEXT_PX_SMALL, FB_W);
        draw_text(fb, MINI_SIDE_CX - text_width(fwd_lbl, TEXT_PX_SMALL) / 2,
                  cy - TEXT_PX_SMALL / 2 + 2, fwd_lbl, COL_TEXT, TEXT_PX_SMALL, FB_W);
    } else if (!radio_mode) {
        /* Next track: triangle against a bar, the same glyph the full
         * player uses for its own next button -- a lone triangle here read
         * as a second play button rather than "skip". Plain white, no
         * circle, same reasoning as the audiobook buttons above. */
        int nh = 22, ntw = (nh * 87) / 100;
        fill_triangle(fb, MINI_SIDE_CX - ntw / 2, cy, nh, +1, COL_TEXT);
        fill_rect(fb, MINI_SIDE_CX + ntw / 2 + 4, cy - nh / 2, 4, nh, COL_TEXT);
    }
}

/* Outline, level, and the nub on the end. Turns red when it is nearly out and
 * accent-coloured on charge, so the state reads without counting digits. A
 * filled rounded rect punched out by a slightly smaller one in the header
 * colour, the same trick as the pill sliders, rather than four traced edges. */
/* Five discrete fill levels (icon_batt_0/25/50/75/100), not a continuous
 * procedural fill -- the FA glyph reads far better at this pixel size than
 * the hand-drawn rounded-rect ever did, and every call site already prints
 * the exact percentage as text alongside the icon, so nothing is lost:
 * the number still carries the precision, the icon now just looks right. */
static void draw_battery(uint16_t *fb, int x, int y, int pct, int charging) {
    const icon_t *ic = pct > 87 ? &icon_batt_100 : pct > 62 ? &icon_batt_75
                      : pct > 37 ? &icon_batt_50  : pct > 12 ? &icon_batt_25
                      : &icon_batt_0;
    uint16_t c = charging ? COL_ACCENT : (pct >= 0 && pct <= 15 ? RGB(230, 80, 70) : COL_DIM);
    draw_icon(fb, FB_W, FB_H, x, y - 1, ic, c);
}

/* Three bars and a play triangle: the queue, in the place a hamburger would
 * sit. It replaced the word QUEUE, which alongside a title reading "Now
 * playing" was two labels telling you what you could already see. */
static void draw_queue_icon(uint16_t *fb, int x, int y, uint16_t c) {
    fill_rect(fb, x, y,      26, 3, c);
    fill_rect(fb, x, y + 8,  26, 3, c);
    fill_rect(fb, x, y + 16, 15, 3, c);
    fill_triangle(fb, x + 23, y + 17, 13, +1, c);
}

/* Body and cone, proportioned to match -- the version this replaced paired a
 * 16px triangle with a 7px box. Two shapes, same as the bluetooth glyph. */
static void draw_speaker(uint16_t *fb, int x, int y, uint16_t c) {
    fill_rect(fb, x, y - 5, 5, 10, c);
    fill_triangle(fb, x + 10, y, 14, -1, c);
}

/* Volume on the left, battery on the right — the same split every other
 * screen uses for its own left margin vs. right-aligned action, rather than
 * both clusters crowded together on one side. */
static void draw_status(uint16_t *fb) {
    char buf[16];
    /* One shared centre line. Everything here is positioned from it rather
     * than from its own top edge, which is what left the speaker sitting
     * above the digits next to it. */
    const int mid = STATUS_H / 2;
    const int ty  = mid - TEXT_PX_SMALL / 2;      /* text box is TEXT_PX_SMALL tall */
    /* Volume and battery only. The headphone and Bluetooth icons both said
     * what the route already says at the foot of the player. */

    int vol = audio_volume();
    const icon_t *vic = vol <= 0 ? &icon_vol_mute : vol < 34 ? &icon_vol_low
                       : vol < 67 ? &icon_vol_mid  : &icon_vol_high;
    draw_icon(fb, FB_W, FB_H, 24, mid - vic->h / 2, vic, COL_DIM);
    snprintf(buf, sizeof(buf), "%d%%", vol);
    draw_text(fb, 24 + 26, ty, buf, COL_DIM, TEXT_PX_SMALL, FB_W - 24 - 26);

    int pct = st_battery_pct();
    int bx = FB_W - 18 - 28;
    draw_battery(fb, bx, mid - 6, pct, st_charging());
    if (pct >= 0) {
        snprintf(buf, sizeof(buf), "%d%%", pct);
        int tw = text_width(buf, TEXT_PX_SMALL);
        draw_text(fb, bx - 8 - tw, ty, buf, COL_DIM, TEXT_PX_SMALL, FB_W);
    }

    fill_rect(fb, 0, STATUS_H - 1, FB_W, 1, COL_LINE);
}

/* Word-wrapped, scrollable show notes -- drawn in place of the cover art
 * within the same box while pod_notes_showing is set. Wrapped fresh at every
 * draw off text_width() rather than pre-computed: the notes buffer is at
 * most 8KB and this is the only place in the app that ever wraps a paragraph,
 * so a real line-layout cache would be more code than the cost it avoids. */
static void pod_draw_notes(uint16_t *fb, int x, int y, int w, int h) {
    fill_rect(fb, x, y, w, h, COL_ROW);
    /* BG50: 20% bigger than TEXT_PX_SMALL (22 -> 26), line height scaled to
     * match rather than left at the old 30px, which read as cramped once
     * the glyphs themselves grew. */
    const int lh = 36, px = 26;
    int ty = y + 16 - pod_notes_scroll_px;
    const char *p = pod_notes_text;
    while (*p) {
        while (*p == ' ') p++;
        if (*p == '\n') { p++; ty += lh; continue; }   /* blank source line stays blank */
        char line[256];
        line[0] = '\0';
        for (;;) {
            const char *ws = p;
            while (*p && *p != ' ' && *p != '\n') p++;
            int wlen = (int)(p - ws);
            if (wlen == 0) break;
            char probe[300];
            if (line[0]) snprintf(probe, sizeof(probe), "%s %.*s", line, wlen, ws);
            else         snprintf(probe, sizeof(probe), "%.*s", wlen, ws);
            if (line[0] && text_width(probe, px) > w - 32) { p = ws; break; }   /* rewind: this word starts the next line */
            snprintf(line, sizeof(line), "%s", probe);
            if (*p == '\n') { p++; break; }
            while (*p == ' ') p++;
            if (!*p) break;
        }
        if (line[0] && ty + lh > y && ty < y + h)
            draw_text_clip(fb, x + 16, ty, line, COL_TEXT, px, w - 32, y, y + h);
        ty += lh;
        if (ty > y + h + 400) break;   /* far enough past the bottom to stop wrapping the rest */
    }
}

static void draw_screen(uint16_t *fb) {
    fill_rect(fb, 0, 0, FB_W, FB_H, COL_BG);
    /* The player has no title bar at all: a strip saying "Now playing" over a
     * screen showing the track, the artist and the artwork was telling you
     * what you could already see, and it was 62px that the artwork wanted.
     * Now Playing also drops the status strip itself (volume, battery) so the
     * art can run edge-to-edge from the very top, like stock -- the battery
     * figure moves to the output-route line at the bottom, volume is simply
     * gone (it was never anything more than a placeholder there), and the
     * Bluetooth codec/headset-battery readout that used to share that line
     * moved into the quick-settings Bluetooth row instead. */
    if (screen != SC_PLAYING) {
        fill_rect(fb, 0, 0, FB_W, CONTENT_Y, COL_HEADER);
    }

    const char *title = "Main Menu";
    const char *right = "EXIT";
    if (screen == SC_ARTISTS) { title = cur_facet_label; right = "BACK"; }
    else if (screen == SC_ALBUMS)  {
        title = recent_mode == RECENT_ADDED ? "Recently added"
              : recent_mode == RECENT_HEARD ? "Recently heard"
              : !cur_artist[0] ? "Albums"
              : cur_artist[0] == LIB_UNKNOWN_MARK[0] ? "Unknown" : cur_artist;
        right = "BACK";
    }
    else if (screen == SC_TRACKS)  { title = cur_album;  right = "BACK"; }
    /* The album being played is the queue, so "QUEUE" goes back to the track
     * list — the same list, with the playing row marked. */

    else if (screen == SC_RADIO)   { title = "Radio"; right = "BACK"; }
    else if (screen == SC_PLAYLISTS) { title = "Playlists"; right = "BACK"; }
    else if (screen == SC_AUDIOBOOKS) { title = "Audiobooks"; right = "BACK"; }
    else if (screen == SC_PODCASTS)   { title = "Podcasts"; right = "BACK"; }
    else if (screen == SC_POD_SYNC)   { title = "Updating feeds"; right = "BACK"; }
    else if (screen == SC_EQ)         { title = "Parametric EQ"; right = "BACK"; }
    else if (screen == SC_EQ_BANDS)   { title = "Bands"; right = "BACK"; }
    else if (screen == SC_MSEB)       { title = "MSEB"; right = "BACK"; }
    else if (screen == SC_EQ_BAND) {
        static char band_title[16];   /* draw_screen's own `buf` isn't declared this early */
        snprintf(band_title, sizeof(band_title), "Band %d", eq_editing_band + 1);
        title = band_title; right = "BACK";
    }
    else if (screen == SC_SETTINGS)       { title = "Settings"; right = "BACK"; }
    else if (screen == SC_SETTINGS_THEME) { title = "Accent colour"; right = "BACK"; }
    else if (screen == SC_SETTINGS_ABOUT) { title = "About"; right = "BACK"; }
    else if (screen == SC_MUSIC_MENU)     { title = "Music"; right = "BACK"; }

    /* The player has no title bar. Drawn unconditionally, it sat behind the
     * artwork with the ends of "Music" and "EXIT" poking out either side of
     * the cover. */
    if (screen != SC_PLAYING) {
        int rw = text_width(right, TEXT_PX_SMALL);
        draw_text(fb, 18, STATUS_H + 14, title, COL_TEXT, TEXT_PX_TITLE, FB_W - rw - 40);
        draw_text(fb, FB_W - 24 - rw, STATUS_H + 20, right, COL_DIM, TEXT_PX_SMALL, FB_W);
        /* MSEB's one extra header action: zero every band back to 0 dB.
         * Doesn't touch Enabled -- "reset" clears the tuning, not the
         * on/off state, which the user didn't ask to lose. */
        if (screen == SC_MSEB)
            draw_text(fb, mseb_reset_x(), STATUS_H + 20, "Reset", COL_DIM, TEXT_PX_SMALL, FB_W);
        /* Podcasts' one extra header action: run .podsync/podsync_once.sh
         * for every feed. Dimmed rather than hidden while it's already
         * running, same convention a disabled control uses elsewhere in
         * this app, since tapping it again would just fork a second sync
         * over the first one's half-written files. */
        if (screen == SC_PODCASTS) {
            draw_text(fb, pod_sync_x(), STATUS_H + 20,
                      pod_update_running() ? "Syncing" : "Sync",
                      pod_update_running() ? COL_DIM : COL_ACCENT, TEXT_PX_SMALL, FB_W);
        }
        draw_status(fb);
        fill_rect(fb, 0, CONTENT_Y - 1, FB_W, 1, COL_LINE);
    }

    /* Rows draw at fixed positions and never shift for scroll_px: a page
     * flip, not a slide. Reading scroll_px here was the actual bug behind
     * "still scrolling as before" — dirty being gated on a row completing
     * did not stop OTHER redraws (the clock, art loading, anything else
     * that marks dirty) from painting mid-drag using whatever fractional
     * scroll_px happened to be at that instant, which looked exactly like
     * continuous scrolling. Ignoring it here is what actually makes rows
     * hold still until a full row's worth of drag completes. */
    int y = CONTENT_Y;
    int clip_bot = FB_H - (mini_visible() ? MINI_H : 40);
    char buf[32];

    if (screen == SC_MENU) {
        /* TEMP: log only when it actually changes from what was last seen,
         * so a long natural session leaves a trail showing exactly when
         * (not just that) the last entry stops being "Settings", instead of
         * either a single boot-time snapshot or a flood on every redraw.
         * TOP_N-1, not a hardcoded index: MSEB's insertion already moved
         * this once, from 4 to 5, and a literal index silently starts
         * watching the wrong slot every time the menu grows again. */
        static const void *dbg_last;
        if (top_menu[TOP_N - 1].label != dbg_last) {
            dbg_last = top_menu[TOP_N - 1].label;
            for (int i = 0; i < TOP_N; i++)
                mlog("[dbg] top_menu[%d] ptr=%p str=\"%s\"\n",
                     i, (void *)top_menu[i].label, top_menu[i].label);
        }
        for (int i = 0; i < TOP_N; i++) {
            draw_text(fb, 24, y + 20, top_menu[i].label, COL_TEXT, TEXT_PX_BODY, FB_W - 24);
            fill_rect(fb, 0, y + ROW_H - 1, FB_W, 1, COL_LINE);
            y += ROW_H;
        }
        return;
    }

    if (screen == SC_MUSIC_MENU) {
        for (int i = 0; i < MENU_N; i++) {
            draw_text(fb, 24, y + 20, menu[i].label, COL_TEXT, TEXT_PX_BODY, FB_W - 24);
            fill_rect(fb, 0, y + ROW_H - 1, FB_W, 1, COL_LINE);
            y += ROW_H;
        }
        return;
    }

    if (screen == SC_PLAYLISTS) {
        /* Clipped top and bottom so a row locked to a letter (index_lock_end)
         * never paints over the header above or the mini player below. */
        for (int i = 0; i < vis_rows(); i++) {
            int idx = scroll + i;
            if (idx >= playlist_n) break;
            draw_text_clip(fb, 24, y + 20, playlists[idx].name, COL_TEXT, TEXT_PX_BODY,
                           FB_W - 40, CONTENT_Y, clip_bot);
            fill_rect_clip(fb, 0, y + ROW_H - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);
            y += ROW_H;
        }
        if (playlist_n == 0)
            draw_text(fb, 24, y + 20, "No playlists on the card", COL_DIM,
                      TEXT_PX_BODY, FB_W - 40);
        if (mini_visible()) draw_mini(fb);
        return;
    }

    if (screen == SC_RADIO) {
        for (int i = 0; i < vis_rows(); i++) {
            int idx = scroll + i;
            if (idx >= station_n) break;
            int playing = radio_mode && audio_is_active() &&
                          !strcmp(stations[idx].name, radio_name);
            if (playing) {
                fill_rect_clip(fb, 0, y, FB_W, ROW_H, COL_ROW, CONTENT_Y, clip_bot);
                fill_rect_clip(fb, 0, y, 4, ROW_H, COL_ACCENT, CONTENT_Y, clip_bot);
            }
            /* A station with no URL is shown dimmed rather than hidden: it is
             * a slot in the file waiting to be filled in. */
            int have = stations[idx].url[0] != '\0';
            draw_text_clip(fb, 24, y + 20, stations[idx].name,
                           playing ? COL_ACCENT : (have ? COL_TEXT : COL_DIM),
                           TEXT_PX_BODY, FB_W - 40, CONTENT_Y, clip_bot);
            fill_rect_clip(fb, 0, y + ROW_H - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);
            y += ROW_H;
        }
        if (station_n == 0)
            draw_text(fb, 24, y + 20, "No stations configured", COL_DIM, TEXT_PX_BODY, FB_W - 40);
        /* A station that plays nothing because Wi-Fi is off looks exactly like
         * a station that is broken. Say which. */
        if (!st_net_up())
            draw_text(fb, 24, FB_H - 34, "Wi-Fi is off", RGB(230, 80, 70), TEXT_PX_SMALL, FB_W - 40);
        else if (radio_msg[0])
            draw_text(fb, 24, FB_H - 34, radio_msg, RGB(230, 80, 70), TEXT_PX_SMALL, FB_W - 40);
        if (mini_visible()) draw_mini(fb);
        return;
    }

    if (screen == SC_PLAYING) {
        if (audiobook_mode) {
            lib_track_t *t = (cur_track >= 0 && cur_track < queue_n) ? &queue[cur_track] : NULL;
            if (!t) return;
            int cy = 0, cx = (FB_W - ART_PX) / 2;
            fill_rect(fb, cx, cy, ART_PX, ART_PX, COL_ROW);
            blit_art(fb, cx, cy);
            int ty = title_y();
            draw_scroll_title(fb, ty, t->name);

            draw_text(fb, 24, ty + 44, q_album, COL_DIM, TEXT_PX_BODY, FB_W - 24);
            snprintf(buf, sizeof(buf), "%d of %d", cur_track + 1, queue_n);
            draw_right(fb, ty + 44, buf);

            /* Positions come from the chapter table, not from summing whole
             * files: a chapter is a stretch of a file, so "where in the
             * chapter" is the file position minus where the chapter starts. */
            const ab_chapter_t *ch =
                (cur_track >= 0 && cur_track < ab_book.chap_n) ? &ab_book.chap[cur_track] : NULL;
            int64_t book_total = ab_book.total_ms;
            int chap_dur = ch ? (int)ch->dur_ms : 0;
            /* A format the scan could not measure has no duration until the
             * decoder reports one; one file then means one chapter, so the
             * file's length is the book's. */
            if (chap_dur <= 0) {
                chap_dur = audio_dur_ms();
                if (book_total <= 0) book_total = chap_dur;
            }
            int64_t chap_start = ch ? ch->file_start_ms : 0;
            int chap_pos = scrub_active ? scrub_ms(chap_dur)
                                        : (int)(audio_pos_ms() - chap_start);
            if (chap_pos < 0) chap_pos = 0;
            int64_t book_pos = (ch ? ch->book_start_ms : 0) + chap_pos;

            /* Real-world time, not content time: at 1.3x, thirty minutes of
             * book is twenty-three minutes away, and that is the number
             * worth showing on a countdown. Position and the bar fill both
             * stay in content time -- unaffected by speed, and what the
             * scrub math already assumes. */
            double speed = ab_speed_permille / 1000.0;

            int bby = ab_book_bar_y();
            draw_text(fb, 24, bby - 26, "Book", COL_DIM, TEXT_PX_SMALL, FB_W - 200);
            if (book_total > 0) {
                fmt_left(buf, sizeof(buf), (int64_t)((book_total - book_pos) / speed));
                draw_right(fb, bby - 26, buf);
            }
            fill_rect(fb, 24, bby, FB_W - 48, 4, COL_LINE);
            if (book_total > 0) {
                int w = (int)((FB_W - 48) * book_pos / book_total);
                if (w > FB_W - 48) w = FB_W - 48;
                fill_rect(fb, 24, bby, w, 4, COL_DIM);   /* dim: display-only, not the accent bar */
            }

            int cby = ab_chapter_bar_y();
            draw_text(fb, 24, cby - 26, "Chapter", COL_DIM, TEXT_PX_SMALL, FB_W - 200);
            if (chap_dur > 0) {
                fmt_left(buf, sizeof(buf), (int64_t)((chap_dur - chap_pos) / speed));
                draw_right(fb, cby - 26, buf);
            }
            int cbh = scrub_active ? 10 : 6;
            int cbyy = cby - (cbh - 6) / 2;
            fill_rect(fb, 24, cbyy, FB_W - 48, cbh, COL_LINE);
            if (chap_dur > 0) {
                int w = (FB_W - 48) * chap_pos / chap_dur;
                if (w > FB_W - 48) w = FB_W - 48;
                fill_rect(fb, 24, cbyy, w, cbh, COL_ACCENT);
                if (scrub_active) fill_circle(fb, 24 + w, cby + 3, 13, COL_ACCENT);
            }

            int cyy = cby + 58;
            int mid = FB_W / 2;

            /* -10s / +10s, not the regular player's prev/next glyphs: a
             * curved skip-arrow with the number inside, the way the design
             * mockup drew it -- a seek amount isn't inferable from a plain
             * triangle, and this is what it actually agreed to. */
            {
                /* Closer to the play button than the regular player's
                 * prev/next (128) sat -- pulled in so the row reads as one
                 * cluster with the speed ring rather than two rings out at
                 * the edges with a gap to it. The input handler's hit test
                 * uses this same 96 for its speed-ring position; keep them
                 * in sync if this moves again. */
                int ring_r = 26, off = 96;
                draw_skip_arc(fb, mid - off, cyy, ring_r, 0, COL_TEXT);
                draw_skip_arc(fb, mid + off, cyy, ring_r, 1, COL_TEXT);
                const char *n = "10";
                int nw = text_width(n, TEXT_PX_BODY);
                draw_text(fb, mid - off - nw / 2, cyy - TEXT_PX_BODY / 2 + 2, n, COL_TEXT, TEXT_PX_BODY, FB_W);
                draw_text(fb, mid + off - nw / 2, cyy - TEXT_PX_BODY / 2 + 2, n, COL_TEXT, TEXT_PX_BODY, FB_W);
            }

            fill_circle(fb, mid, cyy, 42, COL_ACCENT);
            if (audio_is_paused()) {
                fill_triangle(fb, mid + 4, cyy, 36, +1, COL_BG);
            } else {
                fill_rect(fb, mid - 15, cyy - 18, 10, 36, COL_BG);
                fill_rect(fb, mid + 5,  cyy - 18, 10, 36, COL_BG);
            }

            /* Speed: a ring (two concentric fills, since fill_circle only
             * does solid) rather than the transport's solid accent circle,
             * so it doesn't read as a fourth transport button. */
            int scx = mid - 96 - 70, scy = cyy;   /* 96 must match the skip rings' offset above */
            fill_circle(fb, scx, scy, 22, COL_LINE);
            fill_circle(fb, scx, scy, 20, COL_BG);
            snprintf(buf, sizeof(buf), "%.1f\xc3\x97", speed);
            int sw = text_width(buf, TEXT_PX_SMALL);
            draw_text(fb, scx - sw / 2, scy - TEXT_PX_SMALL / 2 + 2, buf, COL_TEXT, TEXT_PX_SMALL, FB_W);

            draw_queue_icon(fb, FB_W - 24 - 26, FB_H - 32, COL_DIM);
            /* Same device-battery-in-the-route-line treatment as the regular
             * player -- see the matching comment there, including that the
             * icon replaces the connection-kind word rather than sitting
             * beside it. */
            const char *route_kind = audio_output();
            int devpct = st_battery_pct();
            const icon_t *ric = !strcmp(route_kind, "Bluetooth") ? &icon_bt_sm
                               : !strcmp(route_kind, "USB")      ? &icon_usb_sm : NULL;
            char routebuf[64];
            if (ric && devpct >= 0)
                snprintf(routebuf, sizeof(routebuf), "%d%%", devpct);
            else if (ric)
                routebuf[0] = '\0';
            else if (devpct >= 0)
                snprintf(routebuf, sizeof(routebuf), "%s \xc2\xb7 %d%%", route_kind, devpct);
            else
                snprintf(routebuf, sizeof(routebuf), "%s", route_kind);
            const char *route = routebuf;
            int ow = text_width(route, TEXT_PX_SMALL);
            int riw = ric ? ric->w + 6 : 0;
            int biw = (devpct >= 0) ? 34 : 0;   /* 8px gap + 26px icon, BG34 */
            int route_x = FB_W - 62 - biw - ow;
            if (route[0])
                draw_text(fb, route_x, FB_H - 34, route, COL_DIM, TEXT_PX_SMALL, FB_W);
            if (ric)
                draw_icon(fb, FB_W, FB_H, route_x - riw,
                          FB_H - 34 + TEXT_PX_SMALL / 2 - ric->h / 2, ric, COL_DIM);
            if (devpct >= 0)
                draw_battery(fb, FB_W - 62 - 26, FB_H - 34 + TEXT_PX_SMALL / 2 - 6,
                            devpct, st_charging());
            /* Format from the extension, not lib_format_name(t->format):
             * chapters come from a folder walk, never the SQL index, so
             * t->format is never populated (see audiobook.h) and showing its
             * default would just be a wrong-looking "?". Bitrate likewise has
             * no tag to read -- computed from the file actually on disk. */
            char ext[16];
            const char *dot = strrchr(t->path, '.');
            snprintf(ext, sizeof(ext), "%s", dot && dot[1] ? dot + 1 : "");
            for (char *p2 = ext; *p2; p2++) *p2 = (char)toupper((unsigned char)*p2);
            /* The file's own duration, not the chapter's and not the book's:
             * a single-file book's chapter is far shorter than the file, and
             * a multi-file book's total spans files other than this one --
             * only file_n==1 makes the book total equal to this file. */
            int64_t file_dur = (ab_book.file_n == 1) ? ab_book.total_ms
                                                      : (ch ? ch->dur_ms : 0);
            int kbps = ab_file_bitrate_kbps(t->path, file_dur);
            if (kbps > 0) snprintf(buf, sizeof(buf), "%s  %d kbps", ext, kbps);
            else          snprintf(buf, sizeof(buf), "%s", ext);
            draw_text(fb, 24, FB_H - 34, buf, COL_ACCENT, TEXT_PX_SMALL,
                      FB_W - 62 - ow - riw - 36);
            return;
        }
        if (radio_mode) {
            /* A live stream has no length, no track number and no cover, so
             * the layout drops the parts that would only ever be blank. */
            int cy = 120;
            draw_text(fb, 24, cy, radio_name, COL_TEXT, TEXT_PX_TITLE, FB_W - 48);
            draw_text(fb, 24, cy + 52, "Internet radio", COL_DIM, TEXT_PX_BODY, FB_W - 48);

            int pos = audio_pos_ms();
            snprintf(buf, sizeof(buf), "%d:%02d", pos / 60000, (pos / 1000) % 60);
            draw_text(fb, 24, cy + 110, buf, COL_DIM, TEXT_PX_SMALL, FB_W);
            draw_right(fb, cy + 110, audio_is_active() ? "LIVE" : "stopped");

            int cyy = cy + 190, mid = FB_W / 2;
            fill_circle(fb, mid, cyy, 42, COL_ACCENT);
            if (audio_is_paused()) fill_triangle(fb, mid + 4, cyy, 36, +1, COL_BG);
            else {
                fill_rect(fb, mid - 15, cyy - 18, 10, 36, COL_BG);
                fill_rect(fb, mid + 5,  cyy - 18, 10, 36, COL_BG);
            }

            /* Say which codec actually turned up: an HLS station is AAC, and
             * labelling everything MP3 was simply wrong. */
            snprintf(buf, sizeof(buf), "%s stream  \xc2\xb7  %s",
                     audio_codec()[0] ? audio_codec() : "Live", audio_output());
            draw_text(fb, 24, FB_H - 34, buf, COL_ACCENT, TEXT_PX_SMALL, FB_W - 48);
            return;
        }

        lib_track_t *t = (cur_track >= 0 && cur_track < queue_n) ? &queue[cur_track] : NULL;
        if (!t) return;

        int cy = 0, cx = (FB_W - ART_PX) / 2;
        /* The info icon (drawn further down, in the transport row) toggles
         * show notes in place of the cover art, within the same box -- see
         * pod_draw_notes() and the matching tap zone below. */
        if (podcast_mode && pod_notes_showing) {
            pod_draw_notes(fb, cx, cy, ART_PX, ART_PX);
        } else {
            fill_rect(fb, cx, cy, ART_PX, ART_PX, COL_ROW);
            blit_art(fb, cx, cy);
        }
        int ty = title_y();
        draw_scroll_title(fb, ty, t->name);
        /* The track's own artist wins when it has one -- q_artist is the
         * *album's* artist and can genuinely differ from a track's own tag
         * (BG30: a compilation showed the album's artist for every track
         * instead of each one's real artist). Falls back to q_artist only
         * for a track with no artist tag of its own (e.g. a playlist entry
         * pulled in from elsewhere) so the line is never simply blank.
         * A podcast episode has no artist tag and an intentionally-empty
         * q_artist (see pod_play_episode()), so this line is blank for one
         * rather than showing the feed name twice. */
        draw_text(fb, 24, ty + 44, t->artist[0] ? t->artist : q_artist,
                  COL_DIM, TEXT_PX_BODY, FB_W - 24);
        draw_text(fb, 24, ty + 82, podcast_mode ? cur_feed : q_album, COL_DIM, TEXT_PX_SMALL, FB_W - 110);
        /* Position in the queue, not the track's own number. Those are not the
         * same thing and showing one against the other produced "11 of 3" on a
         * playlist — and "11 of 9" on an album whose numbering has gaps, which
         * this one does. A podcast episode is never queued alongside others
         * (see podcast_mode's comment), so "1 of 1" would say nothing true. */
        if (!podcast_mode) {
            snprintf(buf, sizeof(buf), "%d of %d", cur_track + 1, queue_n);
            draw_right(fb, ty + 82, buf);
        }

        int pos = audio_pos_ms(), dur = audio_dur_ms();
        if (dur <= 0) dur = t->dur_ms;
        /* While scrubbing the bar and the clock follow the finger rather than
         * the decoder, so you can see where you are about to land. */
        if (scrub_active) pos = scrub_ms(dur);
        int by = bar_y();      /* BG40: was its own ty+118, now the shared value */
        int bh = scrub_active ? 10 : 6;
        int byy = by - (bh - 6) / 2;
        fill_rect(fb, 24, byy, FB_W - 48, bh, COL_LINE);
        if (dur > 0) {
            int w = (FB_W - 48) * pos / dur;
            if (w > FB_W - 48) w = FB_W - 48;
            fill_rect(fb, 24, byy, w, bh, COL_ACCENT);
            if (scrub_active)
                fill_circle(fb, 24 + w, by + 3, 13, COL_ACCENT);
        }
        snprintf(buf, sizeof(buf), "%d:%02d", pos / 60000, (pos / 1000) % 60);
        draw_text(fb, 24, by + 14, buf, COL_DIM, TEXT_PX_SMALL, FB_W);
        int rem = dur - pos; if (rem < 0) rem = 0;
        snprintf(buf, sizeof(buf), "-%d:%02d", rem / 60000, (rem / 1000) % 60);
        draw_right(fb, by + 14, buf);

        /* BG40: was +58, tight enough against the clock row above (ends
         * around by+36) that the gap read as uneven next to the bigger one
         * below the buttons. +70 splits the difference more evenly. */
        int cyy = by + 70;                       /* centre line of the transport */
        int mid = FB_W / 2;

        if (podcast_mode) {
            /* BG47 (revised): just two skip arcs -- -10s left of play/pause,
             * +30s right of it, per explicit correction away from the
             * original symmetric +/-10/+/-30 four-button set. Drawn in this
             * app's own arc style (draw_skip_arc(), already used by the
             * audiobook screen's +/-10s). Speed control is unchanged from
             * before: ported from that same audiobook screen's concentric-
             * ring control and cycling behaviour, backed by its own
             * pod_speed_permille rather than ab_speed_permille -- separate
             * modes, no reason a podcast's chosen speed should share state
             * with a book's. POD_SKIP_OFF is shared with the tap handler
             * below so the two cannot drift apart -- same reasoning as
             * bar_y(). */
            draw_skip_arc(fb, mid - POD_SKIP_OFF, cyy, 26, 0, COL_TEXT);
            draw_skip_arc(fb, mid + POD_SKIP_OFF, cyy, 26, 1, COL_TEXT);
            const char *n10 = "10", *n30 = "30";
            int w10 = text_width(n10, TEXT_PX_BODY), w30 = text_width(n30, TEXT_PX_BODY);
            draw_text(fb, mid - POD_SKIP_OFF - w10 / 2, cyy - TEXT_PX_BODY / 2 + 2, n10, COL_TEXT, TEXT_PX_BODY, FB_W);
            draw_text(fb, mid + POD_SKIP_OFF - w30 / 2, cyy - TEXT_PX_BODY / 2 + 2, n30, COL_TEXT, TEXT_PX_BODY, FB_W);

            int scx = pod_speed_x(mid), scy = cyy;
            fill_circle(fb, scx, scy, 22, COL_LINE);
            fill_circle(fb, scx, scy, 20, COL_BG);
            snprintf(buf, sizeof(buf), "%.1f\xc3\x97", pod_speed_permille / 1000.0);
            int sw = text_width(buf, TEXT_PX_SMALL);
            draw_text(fb, scx - sw / 2, scy - TEXT_PX_SMALL / 2 + 2, buf, COL_TEXT, TEXT_PX_SMALL, FB_W);

            /* Show-notes info icon, right of the +30s arc -- moved here
             * from the top-right corner of the cover art per explicit
             * request. Same ring-then-fill trick as the speed control just
             * drawn: two concentric fill_circle()s, since fill_circle only
             * ever draws solid. */
            if (pod_notes_avail) {
                int icx = pod_info_x(mid), icy = cyy;
                fill_circle(fb, icx, icy, 22, COL_LINE);
                fill_circle(fb, icx, icy, 20, COL_BG);
                draw_text(fb, icx - 3, icy - TEXT_PX_BODY / 2 + 2, "i", COL_TEXT, TEXT_PX_BODY, FB_W);
            }
        } else {
            /* previous: triangle against a bar */
            fill_triangle(fb, mid - 128, cyy, 34, -1, COL_TEXT);
            fill_rect(fb, mid - 128 - 15 - 5, cyy - 17, 5, 34, COL_TEXT);
            /* next: mirrored */
            fill_triangle(fb, mid + 128, cyy, 34, +1, COL_TEXT);
            fill_rect(fb, mid + 128 + 15, cyy - 17, 5, 34, COL_TEXT);
        }

        fill_circle(fb, mid, cyy, 42, COL_ACCENT);
        if (audio_is_paused()) {
            /* nudged right: a triangle's visual centre is left of its bounding
             * box, and centring the box makes it look like it is falling over */
            fill_triangle(fb, mid + 4, cyy, 36, +1, COL_BG);
        } else {
            fill_rect(fb, mid - 15, cyy - 18, 10, 36, COL_BG);
            fill_rect(fb, mid + 5,  cyy - 18, 10, 36, COL_BG);
        }

        /* Three things on one line, right to left: the queue control owns the
         * corner, the route sits inside it, and the format gets whatever is
         * left. Run together as one string the route fell off the end. */
        draw_queue_icon(fb, FB_W - 24 - 26, FB_H - 32, COL_DIM);
        /* The Bluetooth codec and headset battery that used to live here moved
         * to the quick-settings Bluetooth row instead -- this line shows the
         * device's own battery now, in the same slot. */
        const char *route_kind = audio_output();   /* "3.5 mm" / "USB" / "Bluetooth" */
        int devpct = st_battery_pct();
        /* The icon already says the connection kind -- printing the word next
         * to it said the same thing twice. Where there's an icon, the text is
         * just the battery percentage (or nothing); "3.5 mm" has no Font
         * Awesome asset in this set, so that route keeps the plain word, as
         * it always has. */
        const icon_t *ric = !strcmp(route_kind, "Bluetooth") ? &icon_bt_sm
                           : !strcmp(route_kind, "USB")      ? &icon_usb_sm : NULL;
        char routebuf[64];
        if (ric && devpct >= 0)
            snprintf(routebuf, sizeof(routebuf), "%d%%", devpct);
        else if (ric)
            routebuf[0] = '\0';
        else if (devpct >= 0)
            snprintf(routebuf, sizeof(routebuf), "%s \xc2\xb7 %d%%", route_kind, devpct);
        else
            snprintf(routebuf, sizeof(routebuf), "%s", route_kind);
        const char *route = routebuf;
        int ow = text_width(route, TEXT_PX_SMALL);
        int riw = ric ? ric->w + 6 : 0;
        /* BG34: a glyph alongside the digits, same as the status bar's own
         * battery reading -- text then icon, right to left. */
        int biw = (devpct >= 0) ? 34 : 0;   /* 8px gap + 26px icon */
        int route_x = FB_W - 62 - biw - ow;
        if (route[0])
            draw_text(fb, route_x, FB_H - 34, route, COL_DIM, TEXT_PX_SMALL, FB_W);
        if (ric)
            draw_icon(fb, FB_W, FB_H, route_x - riw,
                      FB_H - 34 + TEXT_PX_SMALL / 2 - ric->h / 2, ric, COL_DIM);
        if (devpct >= 0)
            draw_battery(fb, FB_W - 62 - 26, FB_H - 34 + TEXT_PX_SMALL / 2 - 6,
                        devpct, st_charging());
        /* A podcast episode comes from a folder walk, never the SQL index --
         * same reasoning as the audiobook screen's own comment here: t->
         * format/bits/rate are never populated for one (see pod_rebuild_
         * tracks()), so track_format_name(t) would show a meaningless
         * "FLAC 0/0 kHz 0 kbps" rather than the extension actually on disk. */
        if (podcast_mode) {
            char ext[16];
            const char *dot = strrchr(t->path, '.');
            snprintf(ext, sizeof(ext), "%s", dot && dot[1] ? dot + 1 : "");
            for (char *p2 = ext; *p2; p2++) *p2 = (char)toupper((unsigned char)*p2);
            /* BG48: dur, not t->dur_ms -- t->dur_ms only ever comes from a
             * saved resume record (see pod_rebuild_tracks()), which is 0 for
             * any episode that has never been played before. `dur` is the
             * same live audio_dur_ms()-with-fallback the position bar above
             * already uses, and it's what's actually known right now. */
            int kbps = ab_file_bitrate_kbps(t->path, dur);
            if (kbps > 0) snprintf(buf, sizeof(buf), "%s  %d kbps", ext, kbps);
            else          snprintf(buf, sizeof(buf), "%s", ext);
        } else {
            snprintf(buf, sizeof(buf), "%s  %d/%g kHz  %d kbps",
                     track_format_name(t), t->bits, t->rate / 1000.0,
                     t->bitrate / 1000);
        }
        draw_text(fb, 24, FB_H - 34, buf, COL_ACCENT, TEXT_PX_SMALL,
                  FB_W - 62 - ow - riw - 36);
        return;
    }

    if (screen == SC_TRACKS) {
        /* Reached from Now Playing this doubles as the queue, so the playing
         * track is marked wherever the list is entered from. */
        for (int i = 0; i < vis_rows(); i++) {
            int idx = scroll + i;
            if (idx >= track_n) break;
            lib_track_t *t = &tracks[idx];
            /* pod_list: cur_track/queue[] identify the playing episode by a
             * one-entry queue (see podcast_mode's comment), not by an index
             * into this freshly-reloaded feed list -- idx == cur_track would
             * just mark whichever episode happens to sort first. Matched by
             * path instead, the same thing a fresh pod_load_episodes() call
             * can't be expected to keep a stable index for. */
            int playing = pod_list
                ? (audio_is_active() && podcast_mode && cur_track == 0 &&
                   queue_n > 0 && t->path[0] && !strcmp(t->path, queue[0].path))
                : (audio_is_active() && idx == cur_track &&
                   !strcmp(cur_album, q_album) && !strcmp(cur_artist, q_artist));
            if (playing) {
                fill_rect_clip(fb, 0, y, FB_W, ROW_H, COL_ROW, CONTENT_Y, clip_bot);
                fill_rect_clip(fb, 0, y, 4, ROW_H, COL_ACCENT, CONTENT_Y, clip_bot);
            }
            /* The number the file states. Blank rather than a dash when the
             * file does not say, which is rare now it is read from tags. */
            if (t->track > 0) snprintf(buf, sizeof(buf), "%d", t->track);
            else              buf[0] = '\0';
            draw_text_clip(fb, 20, y + 22, buf, COL_DIM, TEXT_PX_SMALL, 56, CONTENT_Y, clip_bot);
            /* FB_W - 110, matching every other row in this file that reserves
             * space for a short right-aligned figure (album/track counts):
             * index_visible() is false on this screen, so draw_right_clip's
             * duration text right-aligns to FB_W-24 with no index-strip
             * allowance, and a duration string never needs more than that.
             * This was FB_W-150, leaving ~76px of clipped title unused for
             * no reason (BG10). */
            /* Per BACKLOG.md's L5 entry: dimmed = not downloaded yet, the
             * same visual language a disabled control uses elsewhere in
             * this app, so the list reads at a glance which rows play and
             * which only start a download. */
            draw_text_clip(fb, 68, y + 20, t->name,
                          playing ? COL_ACCENT
                          : (pod_list && !pod_eps[idx].downloaded) ? COL_DIM
                          : COL_TEXT,
                          TEXT_PX_BODY, FB_W - 110,
                          CONTENT_Y, clip_bot);
            /* pod_list: an episode not yet downloaded has no duration to
             * show (t->dur_ms is 0, same as any file that isn't there yet)
             * -- show the download affordance in that slot instead, or its
             * live progress if this is the one downloading right now. */
            if (pod_list && !pod_eps[idx].downloaded) {
                if (pod_download_active() && pod_download_slot() == idx) {
                    long tot = pod_download_total();
                    if (tot > 0)
                        snprintf(buf, sizeof(buf), "%ld%%", pod_download_bytes() * 100 / tot);
                    else
                        snprintf(buf, sizeof(buf), "%ld KB", pod_download_bytes() / 1024);
                } else {
                    snprintf(buf, sizeof(buf), "Download");
                }
                draw_right_clip(fb, y + 22, buf, CONTENT_Y, clip_bot);
            } else if (t->dur_ms > 0) {
                fmt_dur(buf, sizeof(buf), t->dur_ms);
                draw_right_clip(fb, y + 22, buf, CONTENT_Y, clip_bot);
            }
            fill_rect_clip(fb, 0, y + ROW_H - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);
            y += ROW_H;
        }
        if (sheet_note[0] && !mini_visible()) {
            draw_text(fb, 24, FB_H - 34, sheet_note, COL_ACCENT, TEXT_PX_SMALL, FB_W - 48);
        } else if (track_n > 0 && !mini_visible() && !ab_list && !pod_list) {
            /* Chapters (audiobook_mode) reuses this screen but never the SQL
             * index -- t->format/bits/rate are never populated for them (see
             * audiobook.h), so this line would show a meaningless "FLAC
             * 0/0 kHz" rather than nothing. */
            lib_track_t *t = &tracks[0];
            snprintf(buf, sizeof(buf), "%s  %d/%g kHz",
                     track_format_name(t), t->bits, t->rate / 1000.0);
            draw_text(fb, 24, FB_H - 34, buf, COL_ACCENT, TEXT_PX_SMALL, FB_W - 24);
        }
        return;
    }

    if (screen == SC_POD_SYNC) {
        draw_text(fb, 24, y + 6, pod_update_running() ? "Syncing every feed..."
                                : pod_update_died()    ? "Sync stopped early."
                                                        : "Sync finished.",
                  pod_update_died() ? RGB(230, 80, 70) : COL_DIM, TEXT_PX_BODY, FB_W - 48);
        y += 46;
        fill_rect(fb, 0, y - 1, FB_W, 1, COL_LINE);
        for (int i = 0; i < pod_sync_log_n; i++) {
            draw_text_clip(fb, 24, y + 14, pod_sync_log[i], COL_TEXT, TEXT_PX_SMALL,
                           FB_W - 48, CONTENT_Y, clip_bot);
            y += 40;
        }
        if (pod_sync_log_n == 0)
            draw_text(fb, 24, y + 14, "Starting...", COL_DIM, TEXT_PX_SMALL, FB_W - 48);
        return;
    }

    if (screen == SC_EQ) {
        int ry = eq_row_enabled_y();
        draw_text(fb, 24, ry + 24, "Enabled", COL_TEXT, TEXT_PX_BODY, FB_W - 140);
        draw_toggle_switch(fb, ry, eq_enabled());
        fill_rect(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE);

        ry = eq_row_profile_y();
        draw_text(fb, 24, ry + 24, "Profile", COL_TEXT, TEXT_PX_BODY, FB_W - 200);
        draw_right(fb, ry + 24, eq_cur_path[0] ? eq_cur.name : "None");
        fill_rect(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE);

        int py = eq_preamp_y();
        int pby = py + 26, pbw = FB_W - 48;
        /* While the finger is down on this bar, show where it actually is
         * rather than the last committed value -- same trick the scrub bar
         * uses (scrub_ms() against live_x), so the slider visibly tracks the
         * drag instead of only jumping on release. */
        float shown_preamp = eq_cur.preamp_db;
        if (eq_dragging == 1) {
            int px = live_x - 24; if (px < 0) px = 0; if (px > pbw) px = pbw;
            shown_preamp = -12.0f + 18.0f * (float)px / (float)pbw;
        }
        draw_text(fb, 24, py, "Preamp", COL_DIM, TEXT_PX_SMALL, FB_W - 160);
        snprintf(buf, sizeof(buf), "%.1f dB", shown_preamp);
        draw_right(fb, py, buf);
        fill_pill(fb, 24, pby, pbw, 6, COL_LINE);
        float pt = (shown_preamp + 12.0f) / 18.0f;   /* -12..+6 dB */
        if (pt < 0) pt = 0; if (pt > 1) pt = 1;
        int pw = (int)(pbw * pt);
        if (pw > 0) fill_pill(fb, 24, pby, pw, 6, COL_ACCENT);
        fill_circle(fb, 24 + pw, pby + 3, 12, COL_ACCENT);

        /* Response curve: real, from the live coefficients, not illustrative --
         * eq_response_db() is the same evaluation eq_process_* is built on. */
        int cy = eq_curve_y(), cgw = FB_W - 48, cgh = 50;
        float freqs[28], resp[28];
        for (int i = 0; i < 28; i++) freqs[i] = eq_freq_from_t((float)i / 27.0f);
        eq_response_db(freqs, resp, 28);
        fill_rect(fb, 24, cy + cgh / 2, cgw, 1, COL_LINE);
        int lx = -1, ly = -1;
        for (int i = 0; i < 28; i++) {
            float d = resp[i];
            if (d > 18.0f) d = 18.0f; else if (d < -18.0f) d = -18.0f;
            int xx = 24 + cgw * i / 27;
            int yy = cy + cgh / 2 - (int)(d / 18.0f * (cgh / 2));
            if (lx >= 0) draw_line(fb, lx, ly, xx, yy, COL_ACCENT);
            lx = xx; ly = yy;
        }

        ry = eq_row_bands_y();
        draw_text(fb, 24, ry + 24, "Bands", COL_TEXT, TEXT_PX_BODY, FB_W - 140);
        snprintf(buf, sizeof(buf), "%d", eq_cur.band_n);
        draw_right(fb, ry + 24, buf);
        fill_rect(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE);

        if (mini_visible()) draw_mini(fb);
        return;
    }

    if (screen == SC_MSEB) {
        int ry = mseb_row_enabled_y();
        draw_text(fb, 24, ry + 24, "Enabled", COL_TEXT, TEXT_PX_BODY, FB_W - 140);
        draw_toggle_switch(fb, ry, mseb_on);
        fill_rect(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE);

        int span = FB_W - 48;
        for (int i = 0; i < MSEB_BAND_N; i++) {
            int by = mseb_band_row_y(i);
            /* Same live-tracking trick as the preamp slider on SC_EQ: while
             * this band is being dragged, show where the finger actually is
             * rather than the last committed value. */
            float g = mseb_gain[i];
            if (mseb_dragging == i) {
                int px = live_x - 24; if (px < 0) px = 0; if (px > span) px = span;
                g = -12.0f + 24.0f * (float)px / (float)span;
            }
            uint16_t c = mseb_on ? COL_TEXT : COL_DIM;
            draw_text(fb, 24, by + 2, EQ_MSEB_BANDS[i].name, c, TEXT_PX_SMALL, span - 100);
            snprintf(buf, sizeof(buf), "%+.1f dB", g);
            draw_right(fb, by + 2, buf);
            draw_text(fb, 24, by + 22, EQ_MSEB_BANDS[i].freq_label, COL_DIM, TEXT_PX_SMALL, span);

            int sy = by + 46;
            fill_pill(fb, 24, sy, span, 6, COL_LINE);
            fill_rect(fb, 24 + span / 2, sy - 4, 1, 14, COL_DIM);
            float t = (g + 12.0f) / 24.0f;
            if (t < 0) t = 0; if (t > 1) t = 1;
            int px = (int)(span * t), cx = span / 2;
            uint16_t fillc = mseb_on ? COL_ACCENT : COL_DIM;
            if (px > cx)      fill_pill(fb, 24 + cx, sy, px - cx, 6, fillc);
            else if (px < cx) fill_pill(fb, 24 + px, sy, cx - px, 6, fillc);
            fill_circle(fb, 24 + px, sy + 3, 11, fillc);

            if (i < MSEB_BAND_N - 1)
                fill_rect(fb, 0, by + MSEB_ROW_H - 1, FB_W, 1, COL_LINE);
        }
        return;
    }

    if (screen == SC_EQ_BANDS) {
        for (int i = 0; i < vis_rows(); i++) {
            int idx = scroll + i;
            if (idx >= eq_cur.band_n) break;
            eq_band_t *b = &eq_cur.band[idx];
            const char *tn = b->type == EQ_PEAK ? "Peak" :
                            b->type == EQ_LOW_SHELF ? "Low shelf" : "High shelf";
            snprintf(buf, sizeof(buf), "Band %d \xc2\xb7 %s", idx + 1, tn);
            draw_text_clip(fb, 24, y + 14, buf, b->on ? COL_TEXT : COL_DIM,
                          TEXT_PX_BODY, FB_W - 110, CONTENT_Y, clip_bot);
            snprintf(buf, sizeof(buf), "%.0f Hz  \xc2\xb7  %+.1f dB  \xc2\xb7  Q %.2f",
                    b->fc, b->gain_db, b->q);
            draw_text_clip(fb, 24, y + 42, buf, COL_DIM, TEXT_PX_SMALL,
                          FB_W - 110, CONTENT_Y, clip_bot);
            draw_toggle_switch(fb, y, b->on);
            fill_rect_clip(fb, 0, y + ROW_H - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);
            y += ROW_H;
        }
        if (eq_cur.band_n == 0)
            draw_text(fb, 24, y + 20, "No bands in this profile", COL_DIM, TEXT_PX_BODY, FB_W - 40);
        if (mini_visible()) draw_mini(fb);
        return;
    }

    if (screen == SC_EQ_BAND) {
        eq_band_t *b = (eq_editing_band >= 0 && eq_editing_band < eq_cur.band_n)
                     ? &eq_cur.band[eq_editing_band] : NULL;
        if (!b) return;

        int ey = eq_enabled_y();
        draw_text(fb, 24, ey + 4, "Enabled", COL_TEXT, TEXT_PX_BODY, FB_W - 140);
        draw_toggle_switch(fb, ey - 12, b->on);

        int ty = eq_type_y();
        draw_text(fb, 24, ty - 26, "Filter type", COL_DIM, TEXT_PX_SMALL, FB_W - 48);
        int segw = (FB_W - 48) / 3;
        const char *names[3] = { "Low shelf", "Peak", "High shelf" };
        for (int s = 0; s < 3; s++) {
            int sx = 24 + segw * s;
            int selected = (int)b->type == s;
            if (selected) fill_rect(fb, sx + 2, ty, segw - 4, 34, COL_ACCENT);
            int tw2 = text_width(names[s], TEXT_PX_SMALL);
            draw_text(fb, sx + (segw - tw2) / 2, ty + 9, names[s],
                     selected ? COL_BG : COL_DIM, TEXT_PX_SMALL, FB_W);
        }

        int sw = FB_W - 48;

        int fy = eq_freq_y();
        float shown_fc = b->fc;
        if (eq_dragging == 2) {
            int px = live_x - 24; if (px < 0) px = 0; if (px > sw) px = sw;
            shown_fc = eq_freq_from_t((float)px / (float)sw);
        }
        draw_text(fb, 24, fy - 24, "Frequency", COL_DIM, TEXT_PX_SMALL, FB_W - 160);
        snprintf(buf, sizeof(buf), "%.0f Hz", shown_fc);
        draw_right(fb, fy - 24, buf);
        fill_pill(fb, 24, fy, sw, 4, COL_LINE);
        float ft = eq_t_from_freq(shown_fc);
        int fx = 24 + (int)(sw * ft);
        if (fx > 24) fill_pill(fb, 24, fy, fx - 24, 4, COL_ACCENT);
        fill_circle(fb, fx, fy + 2, 11, COL_ACCENT);

        int gy = eq_gain_y();
        float shown_gain = b->gain_db;
        if (eq_dragging == 3) {
            int px = live_x - 24; if (px < 0) px = 0; if (px > sw) px = sw;
            shown_gain = -12.0f + 24.0f * (float)px / (float)sw;
        }
        draw_text(fb, 24, gy - 24, "Gain", COL_DIM, TEXT_PX_SMALL, FB_W - 160);
        snprintf(buf, sizeof(buf), "%+.1f dB", shown_gain);
        draw_right(fb, gy - 24, buf);
        fill_pill(fb, 24, gy, sw, 4, COL_LINE);
        float gt = (shown_gain + 12.0f) / 24.0f;
        if (gt < 0) gt = 0; if (gt > 1) gt = 1;
        int gx = 24 + (int)(sw * gt);
        if (gx > 24) fill_pill(fb, 24, gy, gx - 24, 4, COL_ACCENT);
        fill_circle(fb, gx, gy + 2, 11, COL_ACCENT);

        int qy = eq_q_y();
        float shown_q = b->q;
        if (eq_dragging == 4) {
            int px = live_x - 24; if (px < 0) px = 0; if (px > sw) px = sw;
            shown_q = 0.1f + 9.9f * (float)px / (float)sw;
        }
        draw_text(fb, 24, qy - 24, "Q", COL_DIM, TEXT_PX_SMALL, FB_W - 160);
        snprintf(buf, sizeof(buf), "%.2f", shown_q);
        draw_right(fb, qy - 24, buf);
        fill_pill(fb, 24, qy, sw, 4, COL_LINE);
        float qt = (shown_q - 0.1f) / (10.0f - 0.1f);
        if (qt < 0) qt = 0; if (qt > 1) qt = 1;
        int qx = 24 + (int)(sw * qt);
        if (qx > 24) fill_pill(fb, 24, qy, qx - 24, 4, COL_ACCENT);
        fill_circle(fb, qx, qy + 2, 11, COL_ACCENT);

        if (mini_visible()) draw_mini(fb);
        return;
    }

    if (screen == SC_SETTINGS) {
        /* Each divider belongs between two complete settings -- title *and*
         * its own description -- not between a title and the description
         * explaining that same title. Drawing it at "this row's bottom"
         * put it one slot too early throughout: it cut "Power button lock"
         * off from its own description instead of separating that
         * description from "Idle sleep" below it. Drawn at the top of the
         * row that follows instead, so it always closes off the block above
         * it, whatever that block was. */
        int ry = set_row_lock_y();
        draw_text(fb, 24, ry + 20, "Power button lock", COL_TEXT, TEXT_PX_BODY, FB_W - 140);
        draw_toggle_switch(fb, ry, button_lock_enabled);

        int dy = set_lock_desc_y();
        draw_text(fb, 24, dy, "Double-press power to lock the screen and", COL_DIM, TEXT_PX_SMALL, FB_W - 48);
        draw_text(fb, 24, dy + 26, "disable buttons. Double-press again to undo.", COL_DIM, TEXT_PX_SMALL, FB_W - 48);

        ry = set_row_autooff_y();
        fill_rect(fb, 0, ry - 1, FB_W, 1, COL_LINE);
        draw_text(fb, 24, ry + 20, "Auto shutdown", COL_TEXT, TEXT_PX_BODY, FB_W - 200);
        if (auto_off_minutes() == 0) snprintf(buf, sizeof(buf), "Never");
        else                         snprintf(buf, sizeof(buf), "%d min", auto_off_minutes());
        draw_right(fb, ry + 20, buf);

        int ay = set_autooff_desc_y();
        draw_text(fb, 24, ay, "Powers the device off when locked with", COL_DIM, TEXT_PX_SMALL, FB_W - 48);
        draw_text(fb, 24, ay + 26, "nothing playing. Tap to change.", COL_DIM, TEXT_PX_SMALL, FB_W - 48);

        ry = set_row_theme_y();
        fill_rect(fb, 0, ry - 1, FB_W, 1, COL_LINE);
        draw_text(fb, 24, ry + 20, "Accent colour", COL_TEXT, TEXT_PX_BODY, FB_W - 200);
        draw_right(fb, ry + 20, ACCENT_PRESETS[g_accent_idx].name);

        /* R26: takes over closing the list -- Accent colour no longer does,
         * now that it has a row below it. */
        ry = set_row_about_y();
        fill_rect(fb, 0, ry - 1, FB_W, 1, COL_LINE);
        draw_text(fb, 24, ry + 20, "About", COL_TEXT, TEXT_PX_BODY, FB_W - 200);
        fill_rect(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE);

        if (mini_visible()) draw_mini(fb);
        return;
    }

    if (screen == SC_SETTINGS_ABOUT) {
        char kernel[64], firmware[32], serial[40], sd_free[24], ram_free[24];
        about_kernel(kernel, sizeof(kernel));
        about_firmware(firmware, sizeof(firmware));
        about_serial(serial, sizeof(serial));
        about_sd_free(sd_free, sizeof(sd_free));
        about_ram_free(ram_free, sizeof(ram_free));

        int ry = CONTENT_Y;
        draw_text(fb, 24, ry + 20, "Library version", COL_TEXT, TEXT_PX_BODY, FB_W - 200);
        draw_right(fb, ry + 20, LIBRARY_VERSION);
        fill_rect(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE);

        ry += ROW_H;
        draw_text(fb, 24, ry + 20, "Firmware version", COL_TEXT, TEXT_PX_BODY, FB_W - 200);
        draw_right(fb, ry + 20, firmware);
        fill_rect(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE);

        ry += ROW_H;
        draw_text(fb, 24, ry + 20, "Kernel version", COL_TEXT, TEXT_PX_BODY, FB_W - 200);
        draw_right(fb, ry + 20, kernel);
        fill_rect(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE);

        ry += ROW_H;
        draw_text(fb, 24, ry + 20, "Serial number", COL_TEXT, TEXT_PX_BODY, FB_W - 200);
        draw_right(fb, ry + 20, serial);
        fill_rect(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE);

        ry += ROW_H;
        draw_text(fb, 24, ry + 20, "SD card", COL_TEXT, TEXT_PX_BODY, FB_W - 200);
        draw_right(fb, ry + 20, sd_free);
        fill_rect(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE);

        ry += ROW_H;
        draw_text(fb, 24, ry + 20, "Memory", COL_TEXT, TEXT_PX_BODY, FB_W - 200);
        draw_right(fb, ry + 20, ram_free);
        fill_rect(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE);

        if (mini_visible()) draw_mini(fb);
        return;
    }

    if (screen == SC_SETTINGS_THEME) {
        for (int i = 0; i < ACCENT_N; i++) {
            int ry = CONTENT_Y + i * ROW_H;
            fill_circle(fb, 44, ry + ROW_H / 2, 14, ACCENT_PRESETS[i].color);
            draw_text(fb, 72, ry + 20, ACCENT_PRESETS[i].name, COL_TEXT, TEXT_PX_BODY, FB_W - 140);
            if (i == g_accent_idx) {
                /* A tick from two line segments, matching the app's other
                 * line-primitive glyphs rather than a font checkmark. */
                int cx = FB_W - 44, cy = ry + ROW_H / 2;
                draw_line(fb, cx - 10, cy, cx - 3, cy + 7, COL_ACCENT);
                draw_line(fb, cx - 3, cy + 7, cx + 10, cy - 8, COL_ACCENT);
            }
            fill_rect(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE);
        }
        if (mini_visible()) draw_mini(fb);
        return;
    }

    for (int i = 0; i < vis_rows(); i++) {
        int absolute = scroll + i;
        lib_row_t *row = row_at(absolute);
        if (!row) break;
        /* Stop drawing at the end of whatever top-level letter the index put
         * us in — a plain jump to a short letter (E: 4 albums, room for 8)
         * as much as a locked subdivision (Es within E). Leaves blank space
         * below rather than padding out with the next letter. */
        if (index_lock_end >= 0 && absolute >= index_lock_end) break;
        draw_text_clip(fb, 24, y + 20, row->name, COL_TEXT, TEXT_PX_BODY, FB_W - 110,
                       CONTENT_Y, clip_bot);
        if (row->count > 0) {
            snprintf(buf, sizeof(buf), "%d", row->count);
            draw_right_clip(fb, y + 22, buf, CONTENT_Y, clip_bot);
        }
        fill_rect_clip(fb, 0, y + ROW_H - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);
        y += ROW_H;
    }
}

/* ---- input --------------------------------------------------------------- */
/* Returns 1 on a tap, with the coordinates; 0 otherwise. Same shape as the
 * Podcasts app: the node is drained every pass so nothing queues against our
 * grab and replays later. */
/* 1 = tap at (ox,oy); 2 = vertical drag, oy carries the distance. A drag has to
 * be distinguished from a tap or every scroll also opens whatever was under the
 * finger. */
#define DRAG_MIN 18

/* 1 = tap, 2 = vertical drag (*oy carries the distance), 3 = swipe in from the
 * left edge (back), 4 = press and hold. The edge test is on where the finger went down,
 * so a horizontal drag started mid-screen is not mistaken for it. */
#define EDGE_ZONE  100
#define EDGE_TRAVEL 60
#define HOLD_MS     550

/* Live state for the back gesture, so the UI can show it happening rather than
 * only reacting once the finger is lifted. */
static int edge_active, edge_travel, edge_y;

/* Live state for the press-and-hold. Timing the press from when the *release*
 * is read does not work: both can arrive in the same polling pass and the hold
 * then measures as instant. The hold is detected while the finger is still
 * down, which is also how a hold is supposed to feel — it fires under your
 * finger rather than when you let go. */
static int touch_down, touch_x, touch_y, touch_moved, hold_fired;
static struct timespec touch_at;

static int read_gesture(int fd, int *ox, int *oy) {
    struct input_event ev;
    static int x, y, down_x = -1, down_y = -1, moved, have_down;
    int out = 0;
    while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_X) {
            x = ev.value;
            live_x = x;
            /* Sideways counts as movement for the hold, though not for the
             * scroll: a slow swipe in from the edge has no vertical travel at
             * all, so tracking only Y made it look like a stationary finger
             * and it opened the hold menu instead of going back. */
            if (have_down && down_x >= 0 && abs(x - down_x) > DRAG_MIN)
                touch_moved = 1;
        }
        else if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_Y) {
            y = ev.value;
            live_y = y;
            if (have_down && down_y >= 0 && abs(y - down_y) > DRAG_MIN) {
                moved = 1;
                touch_moved = 1;
            }
        }
        else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            if (ev.value == 1) {
                down_x = x; down_y = y; moved = 0; have_down = 1;
                edge_active = 0; edge_travel = 0;
                touch_down = 1; touch_x = x; touch_y = y;
                touch_moved = 0; hold_fired = 0;
                clock_gettime(CLOCK_MONOTONIC, &touch_at);
            }
            else if (have_down) {
                have_down = 0;
                edge_active = 0;
                touch_down = 0;
                int dx = x - down_x, dy = y - down_y;
                /* Not while scrubbing, and not with quick settings open. The
                 * progress bar and the brightness slider both start at x=24,
                 * well inside the edge zone, so dragging from the start of
                 * either was indistinguishable from a swipe back — the
                 * scrub bar was fixed for this already; the brightness
                 * slider shared the same geometry and was never added. On
                 * SC_MENU, go_back() returning 0 exits the app outright,
                 * which is what "the screen went black" during a brightness
                 * drag actually was. The EQ preamp/frequency/gain/Q sliders
                 * start at the same x=24 and share the guard for the same
                 * reason. */
                if (!scrub_active && !qs_open && !eq_dragging && !title_dragging && down_x >= 0 && down_x < EDGE_ZONE &&
                    dx > EDGE_TRAVEL && dx > abs(dy)) {
                    out = 3;
                } else if (moved) {
                    out = 2; *oy = dy;
                } else if (hold_fired) {
                    out = 0;              /* the hold already acted; not a tap */
                } else {
                    out = 1; *ox = x; *oy = y;
                }
                down_x = down_y = -1;
            }
        }

        /* Deliberately outside the if/else-if chain above. Written as another
         * branch of it, this swallowed the touch-release event whenever an
         * edge drag was in progress — the gesture never completed, so the
         * hint appeared and stayed put and nothing ever went back. It is not
         * a kind of event; it is an observation about where the finger is. */
        if (!scrub_active && !qs_open && !eq_dragging && !title_dragging && have_down && down_x >= 0 && down_x < EDGE_ZONE) {
            int edx = x - down_x;
            if (edx > 4 && edx > abs(y - down_y)) {
                edge_active = 1;
                edge_travel = edx;
                edge_y = y;
            }
        }
    }
    return out;
}

/* Each screen returns early once it has drawn, so the mini player goes on
 * afterwards rather than being repeated at the foot of every branch. */
/* One definition of "back", so the header control and the edge swipe cannot
 * drift apart. Returns 0 when there is nowhere left to go, which at the top
 * level means leaving the app. */
static int go_back(void) {
    /* Show notes intercept, not guard: closing them has to happen *here*,
     * ahead of everything else below, so both ways into "back" -- the edge
     * swipe and the header BACK control, which both just call go_back() --
     * close the notes first rather than leaving the player under them. Same
     * "one definition of back" property the function comment above is
     * about, just extended to a second thing back can mean on this screen. */
    if (screen == SC_PLAYING && podcast_mode && pod_notes_showing) {
        pod_notes_showing = 0;
        return 1;
    }
    /* Every way out of a screen goes through here, including all the way
     * out of the app (SC_MENU returning 0), so this is the one place that
     * needs to know about leaving rather than every call site. A no-op
     * whenever a book isn't what's playing. Likewise pod_save_current_pos()
     * for an episode. */
    ab_save_current_pos();
    pod_save_current_pos();
    switch (screen) {
        case SC_MENU:
            return 0;
        case SC_PLAYING:
            if (radio_mode) { screen = SC_RADIO; reset_scroll(); break; }
            /* Straight from the queue copy — no second trip to the database,
             * and it is right even if the browser has wandered off. For a
             * book the queue is its chapters, so this is the chapter list. */
            memcpy(tracks, queue, sizeof(queue[0]) * (size_t)queue_n);
            track_n = queue_n;
            snprintf(cur_artist, sizeof(cur_artist), "%s", q_artist);
            snprintf(cur_album,  sizeof(cur_album),  "%s", q_album);
            /* So a further "back" out of this queue view (BG7's fix) restores
             * this same artist rather than whatever an earlier, unrelated
             * Albums browse last left in albums_artist -- but only when we
             * actually got here without a normal Albums browse to fall back
             * on (played_from_browse false). When we did descend normally,
             * albums_artist already holds the right facet-column value and
             * q_artist (a different column, see the field's own comment)
             * would overwrite it with the wrong one. */
            if (!played_from_browse)
                snprintf(albums_artist, sizeof(albums_artist), "%s", q_artist);
            /* q_album is deliberately left empty for a podcast episode (see
             * pod_play_episode()) so the feed name has to come from
             * cur_feed instead -- otherwise SC_TRACKS's header would show a
             * blank title. And unlike a book's chapters, the queue copy above
             * is NOT the full episode list here -- podcast_mode's queue is
             * deliberately just the one playing episode (see its comment), so
             * "back" has to re-fetch the feed rather than mirror the queue,
             * or the list would collapse to a single row. */
            if (podcast_mode) {
                snprintf(cur_album, sizeof(cur_album), "%s", cur_feed);
                pod_ep_n = pod_load_episodes(cur_feed, pod_eps, POD_MAX_ITEMS);
                pod_rebuild_tracks();
            }
            ab_list = audiobook_mode;
            pod_list = podcast_mode;
            pod_notes_showing = 0;
            screen = SC_TRACKS;
            reset_scroll();
            break;
        case SC_RADIO:
        case SC_AUDIOBOOKS:
        case SC_PODCASTS:
        case SC_EQ:
        case SC_MSEB:
        case SC_SETTINGS:
        case SC_MUSIC_MENU:
            screen = SC_MENU; reset_scroll();
            break;
        case SC_PLAYLISTS:
        case SC_ARTISTS:
            /* One level deeper than the rest (L2): these are reached via
             * "Music", not the top-level menu directly. */
            screen = SC_MUSIC_MENU; reset_scroll();
            break;
        case SC_EQ_BANDS:
            screen = SC_EQ; reset_scroll();
            break;
        case SC_POD_SYNC:
            /* Refreshed the same way opening Podcasts does -- a sync that's
             * still running when the reader backs out of watching it may
             * finish moments later with new feeds or episodes on disk that
             * this list hasn't seen yet. */
            screen = SC_PODCASTS; reset_scroll();
            pod_feed_n = pod_scan_feeds(pod_feeds, POD_MAX_FEEDS);
            pod_rebuild_rows();
            total = pod_feed_n;
            break;
        case SC_SETTINGS_THEME:
        case SC_SETTINGS_ABOUT:
            screen = SC_SETTINGS; reset_scroll();
            break;
        case SC_EQ_BAND:
            /* No save here: each slider/toggle already persists itself the
             * moment it's touched (see the tap handlers). A save on the way
             * out too would mean just opening a band and leaving it, with
             * nothing dragged, forks it into a "(Custom)" copy for no
             * reason -- eq_save_current() must only ever fire from an
             * actual edit. */
            screen = SC_EQ_BANDS; reset_scroll();
            total = eq_cur.band_n;
            break;
        case SC_ALBUMS:
            if (cur_facet) {
                screen = SC_ARTISTS; reset_scroll();
                /* Restore the count for the list being returned to. Without
                 * this the footer kept the number from the level below — one
                 * artist's two albums, reported as "1-9 of 2". */
                total = lib_group_count(cur_facet);
                /* BG37: land back where the list was, not at the top.
                 * Clamped in case the library changed shape underneath (a
                 * rescan removed rows) since the artist was tapped. */
                scroll = artists_scroll_saved;
                if (scroll > total - 1) scroll = total > 0 ? total - 1 : 0;
                if (scroll < 0) scroll = 0;
                scroll_px = artists_scroll_px_saved;
                load_page();
            } else {
                /* Reached via "Music" -> Albums, no facet above it. */
                screen = SC_MUSIC_MENU; reset_scroll();
            }
            break;
        default:
            /* SC_TRACKS doubles as the queue view for a regular album AND
             * (when audiobook_mode) as Chapters -- same screen, same data
             * shape, but "back" has to land somewhere that makes sense for
             * whichever it is, not always the SQL album browser. */
            if (pod_list) {
                screen = SC_PODCASTS; reset_scroll();
                pod_feed_n = pod_scan_feeds(pod_feeds, POD_MAX_FEEDS);
                pod_rebuild_rows();
                total = pod_feed_n;
            } else if (ab_list) {
                screen = SC_AUDIOBOOKS; reset_scroll();
                ab_book_n = ab_scan_books(ab_books, AB_MAX_BOOKS);
                ab_rebuild_rows();
                total = ab_book_n;
            } else if (recent_mode) {
                /* Recently added/heard: load_page() is a no-op here --
                 * index_visible() is deliberately false for recent_mode, so
                 * the facet-query path below would leave rows[]/row_n stale
                 * against a freshly wrong `total`, rendering blank. Same
                 * one-shot recompute the menu tap itself used to get here. */
                screen = SC_ALBUMS; reset_scroll();
                row_n = (recent_mode == RECENT_ADDED)
                      ? lib_albums_recent_added(rows, RECENT_ALBUMS_N)
                      : lib_albums_recent_heard(recent_heard_ts, rows, RECENT_ALBUMS_N);
                row_base = 0;
                total = row_n;
            } else {
                /* BG7: tapping an album row overwrites cur_artist with that
                 * one album's own artist (needed so two artists sharing an
                 * album name still get the right tracks) -- restore what
                 * Albums was actually filtered by before that happened,
                 * rather than requerying and redrawing the header with the
                 * single album's artist stuck in place. */
                snprintf(cur_artist, sizeof(cur_artist), "%s", albums_artist);
                screen = SC_ALBUMS; reset_scroll();
                total = lib_albums_count(cur_facet, cur_artist[0] ? cur_artist : NULL);
                /* BG37 follow-up: land back where the list was, not at the
                 * top. Same clamp BG37 uses for SC_ARTISTS, in case the
                 * album count changed underneath (a rescan) since the album
                 * was tapped. */
                scroll = albums_scroll_saved;
                if (scroll > total - 1) scroll = total > 0 ? total - 1 : 0;
                if (scroll < 0) scroll = 0;
                scroll_px = albums_scroll_px_saved;
                load_page();
            }
            break;
    }
    return 1;
}

/* Blend two RGB565 colours. Used only by the back-gesture hint, which wants to
 * fade rather than switch on. */
static uint16_t mix565(uint16_t a, uint16_t b, int t /* 0..256 */) {
    int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    int r = ar + (br - ar) * t / 256;
    int g = ag + (bg - ag) * t / 256;
    int bl = ab + (bb - ab) * t / 256;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

/* A sliver at the left edge that grows and warms as the finger travels, going
 * fully accent-coloured once the swipe is far enough to count. Without it the
 * gesture is invisible until it has already happened, and there is no way to
 * tell a swipe that will register from one that will not. */
#define HINT_MAX_W 8
#define HINT_H     150

static void draw_back_hint(uint16_t *fb) {
    int t = edge_travel * 256 / EDGE_TRAVEL;
    if (t > 256) t = 256;
    int w = HINT_MAX_W * t / 256;
    if (w < 2) w = 2;

    int top = edge_y - HINT_H / 2;
    if (top < CONTENT_Y) top = CONTENT_Y;
    if (top + HINT_H > FB_H) top = FB_H - HINT_H;

    /* Warm late rather than linearly: for most of the travel this should be
     * barely there, and only clearly accent-coloured once the swipe is far
     * enough that letting go will actually go back. */
    uint16_t c = mix565(COL_LINE, COL_ACCENT, t * t / 256);
    /* Tapered ends, so it reads as a soft highlight rather than a bar. */
    for (int i = 0; i < HINT_H; i++) {
        int d = i < HINT_H / 2 ? i : HINT_H - 1 - i;
        int ww = d < 24 ? w * d / 24 : w;
        if (ww > 0) fill_rect(fb, 0, top + i, ww, 1, c);
    }
}

#define SHEET_HEAD 46

static int sheet_rows(void) {
    return sheet_open == 2 ? playlist_n + 1 :
           sheet_open == 3 ? eq_profile_n + 1 : SHEET_N;
}
static int sheet_top(void) { return FB_H - sheet_rows() * SHEET_ROW - SHEET_HEAD; }

static void draw_sheet(uint16_t *fb) {
    int top = sheet_top();
    /* Dim what is behind it so the sheet reads as being in front -- the full
     * screen, status bar and header included (BG22): starting at CONTENT_Y
     * left them undimmed, which read as a shadow that stopped partway up
     * rather than one behind the whole screen. */
    for (int yy = 0; yy < top; yy++)
        for (int xx = 0; xx < FB_W; xx++) {
            uint16_t *px = fb + (size_t)yy * FB_W + xx;
            *px = mix565(*px, COL_BG, 150);
        }

    fill_rect(fb, 0, top, FB_W, FB_H - top, COL_HEADER);
    fill_rect(fb, 0, top, FB_W, 1, COL_LINE);

    /* The caption belongs inside the panel: drawn above it, it landed on top of
     * whichever list row happened to be there. */
    const char *cap = sheet_open == 3 ? "Choose profile"
                    : (sheet_track >= 0 && sheet_track < track_n)
                    ? tracks[sheet_track].name : "";
    draw_text(fb, 24, top + 14, cap, COL_DIM, TEXT_PX_SMALL, FB_W - 48);

    int rows = sheet_rows();
    for (int i = 0; i < rows; i++) {
        int ry = top + SHEET_HEAD + i * SHEET_ROW;
        const char *label;
        int last;
        if (sheet_open == 2) {
            last = (i == playlist_n);
            label = last ? "Cancel" : playlists[i].name;
        } else if (sheet_open == 3) {
            last = (i == eq_profile_n);
            label = last ? "Cancel" : eq_profiles[i].name;
        } else {
            last = (i == SHEET_N - 1);
            label = sheet_items[i];
        }
        draw_text(fb, 24, ry + 16, label, last ? COL_DIM : COL_TEXT,
                  TEXT_PX_BODY, FB_W - 48);
        if (!last) fill_rect(fb, 24, ry + SHEET_ROW - 1, FB_W - 48, 1, COL_LINE);
    }
}

/* Geometry shared by the drawing and the hit tests. */
static int qs_bar_y(void)  { return STATUS_H + 74; }
static int qs_wifi_y(void) { return STATUS_H + 130; }
static int qs_bt_y(void)   { return qs_wifi_y() + QS_ROW_H; }
static int qs_eq_y(void)   { return qs_bt_y() + QS_ROW_H; }
static int qs_mseb_y(void) { return qs_eq_y() + QS_ROW_H; }

static void draw_bt_icon(uint16_t *fb, int x, int y, uint16_t c) {
    draw_icon(fb, FB_W, FB_H, x, y, &icon_bt_qs, c);
}

static void draw_wifi_icon(uint16_t *fb, int x, int y, uint16_t c) {
    draw_icon(fb, FB_W, FB_H, x, y, &icon_wifi_qs, c);
}

/* A small frequency-response squiggle with a control point at each vertex --
 * what SC_EQ draws full-size via eq_response_db(), echoed in miniature. Bars
 * read as "generic audio/volume"; this reads as what the feature actually
 * is, a curve you shape at a few points. */
static void draw_eq_icon(uint16_t *fb, int x, int y, uint16_t c) {
    /* 1.45x the original points/radius, to sit at the same visual weight as
     * the enlarged Wi-Fi/Bluetooth bitmaps in the same list -- see the Quick
     * Settings row comments for why they all changed together. */
    int px[5] = { 0, 9, 17, 26, 35 };
    int py[5] = { 20, 25, 9, 16, 1 };
    for (int i = 0; i < 4; i++)
        draw_line(fb, x + px[i], y + py[i], x + px[i + 1], y + py[i + 1], c);
    for (int i = 0; i < 5; i++)
        fill_circle(fb, x + px[i], y + py[i], 3, c);
}

static void draw_quick_settings(uint16_t *fb) {
    fill_rect(fb, 0, 0, FB_W, QS_H, COL_HEADER);
    fill_rect(fb, 0, QS_H - 1, FB_W, 1, COL_LINE);
    draw_status(fb);

    draw_text(fb, 24, STATUS_H + 12, "Brightness", COL_DIM, TEXT_PX_SMALL, FB_W - 48);
    int by = qs_bar_y(), bw = FB_W - 48;
    fill_pill(fb, 24, by, bw, 8, COL_LINE);
    int filled = qs_bright_max > 0 ? bw * qs_bright / qs_bright_max : 0;
    if (filled > 0) fill_pill(fb, 24, by, filled, 8, COL_ACCENT);
    fill_circle(fb, 24 + filled, by + 4, 13, COL_ACCENT);

    /* Icon, name, and what it is actually attached to — the useful part, and
     * what stock shows. Coloured when on, so state reads without the toggle. */
    char nm[64];
    int wy = qs_wifi_y();
    /* +16, not +12: a bounding-box center undersells this icon's shape.
     * Almost all of its ink is the arcs in the top two-thirds -- the dot is
     * a handful of pixels -- so a box-centered placement put the visible
     * mass level with "Wi-Fi" and left it reading as floating above "off"
     * rather than spanning to it, even though the box itself matched the
     * text block exactly. Placed by alpha-weighted centroid instead (row
     * 11.7 of 34 in the bitmap), matched to Bluetooth's own weighted
     * centroid (row 18 of 38 at its +10 offset -> 28 from the row top). */
    draw_wifi_icon(fb, 24, wy + 16, qs_wifi ? COL_ACCENT : COL_DIM);
    /* QS_LABEL_X, not the old 68: at its new, larger size the Wi-Fi icon's
     * natural width (its source aspect ratio times the target height) puts
     * its right edge exactly at 68 -- zero gap, reading as crowding into the
     * "W". 76 clears it with room to spare. */
    draw_text(fb, QS_LABEL_X, wy + 6, "Wi-Fi", qs_wifi ? COL_TEXT : COL_DIM, TEXT_PX_SMALL, 200);
    st_wifi_ssid(nm, sizeof(nm));
    draw_text(fb, QS_LABEL_X, wy + 32, qs_wifi ? (nm[0] ? nm : "not connected") : "off",
              COL_DIM, TEXT_PX_SMALL, FB_W - 180);
    draw_toggle_switch(fb, wy, qs_wifi);

    int by2 = qs_bt_y();
    /* Block-centered vertically, same as Wi-Fi above. x=34, not the icon
     * column's usual 24-26: measured on a real screenshot, this icon's own
     * alpha-weighted horizontal centroid landed at x=38 against Wi-Fi's 46 --
     * a real, visible 8px gap between the two icons' center lines, not
     * merely a left-edge difference. Left-aligning icons of different
     * natural widths does not make them share a center; +8 here matches
     * this one's centroid to Wi-Fi's rather than its left edge. */
    draw_bt_icon(fb, 34, by2 + 10, qs_bt ? COL_ACCENT : COL_DIM);
    draw_text(fb, QS_LABEL_X, by2 + 6, "Bluetooth", qs_bt ? COL_TEXT : COL_DIM, TEXT_PX_SMALL, 200);
    st_bt_name(nm, sizeof(nm));
    if (qs_bt && nm[0]) {
        /* The codec and the headset's own battery used to sit on the Now
         * Playing screen's route line; they moved here, next to the name they
         * actually describe, freeing that line for the device's own battery. */
        char codec[32]; int batt;
        pthread_mutex_lock(&bt_lock);
        snprintf(codec, sizeof(codec), "%s", bt_codec_cached);
        batt = bt_batt_cached;
        pthread_mutex_unlock(&bt_lock);
        char base[80];
        if (codec[0]) snprintf(base, sizeof(base), "%s \xc2\xb7 %s", nm, codec);
        else          snprintf(base, sizeof(base), "%s", nm);
        draw_text(fb, QS_LABEL_X, by2 + 32, base, COL_DIM, TEXT_PX_SMALL, FB_W - 180);
        if (batt >= 0) {
            int tx = QS_LABEL_X + text_width(base, TEXT_PX_SMALL);
            draw_text(fb, tx, by2 + 32, " \xc2\xb7 ", COL_DIM, TEXT_PX_SMALL, FB_W - 180);
            tx += text_width(" \xc2\xb7 ", TEXT_PX_SMALL);
            draw_battery(fb, tx, by2 + 37, batt, 0);
            tx += 30;
            char pct[8];
            snprintf(pct, sizeof(pct), "%d%%", batt);
            draw_text(fb, tx, by2 + 32, pct, COL_DIM, TEXT_PX_SMALL, FB_W - 180 - (tx - QS_LABEL_X));
        }
    } else {
        draw_text(fb, QS_LABEL_X, by2 + 32, qs_bt ? "not connected" : "off",
                  COL_DIM, TEXT_PX_SMALL, FB_W - 180);
    }
    draw_toggle_switch(fb, by2, qs_bt);

    int by3 = qs_eq_y();
    int on = eq_enabled();
    /* Block-centered vertically, same as Wi-Fi/Bluetooth above (y was +6
     * when the icon was smaller and merely aligned to the title line). x=29
     * for the same centroid-matching reason as Bluetooth's +34 above --
     * measured centroid 42 against Wi-Fi's 46, so +5. */
    draw_eq_icon(fb, 29, by3 + 16, on ? COL_ACCENT : COL_DIM);
    draw_text(fb, QS_LABEL_X, by3 + 6, "Parametric EQ", on ? COL_TEXT : COL_DIM, TEXT_PX_SMALL, 200);
    draw_text(fb, QS_LABEL_X, by3 + 32, eq_cur_path[0] ? eq_cur.name : "no profile",
              COL_DIM, TEXT_PX_SMALL, FB_W - 180);
    draw_toggle_switch(fb, by3, on);

    /* Same shape as the Parametric EQ row above -- a quick toggle, not a way
     * in to editing the 9 bands (that stays under Parametric EQ in the main
     * menu). Reuses the same squiggle icon: MSEB is still, visually, "an
     * EQ" -- a distinct glyph for it would say otherwise. */
    int by4 = qs_mseb_y();
    draw_eq_icon(fb, 29, by4 + 16, mseb_on ? COL_ACCENT : COL_DIM);
    draw_text(fb, QS_LABEL_X, by4 + 6, "MSEB", mseb_on ? COL_TEXT : COL_DIM, TEXT_PX_SMALL, 200);
    draw_text(fb, QS_LABEL_X, by4 + 32, "HiBy tuning bands", COL_DIM, TEXT_PX_SMALL, FB_W - 180);
    draw_toggle_switch(fb, by4, mseb_on);

    /* A grab handle, so it is obvious the panel goes back up. */
    fill_rect(fb, FB_W / 2 - 26, QS_H - 14, 52, 4, COL_LINE);
}

static int vol_bar_y(void) { return STATUS_H + VOL_H / 2 + 6; }

static void draw_volume(uint16_t *fb) {
    int top = STATUS_H;
    fill_rect(fb, 0, top, FB_W, VOL_H, COL_HEADER);
    fill_rect(fb, 0, top + VOL_H - 1, FB_W, 1, COL_LINE);

    int v = vol_dragging && vol_drag_pct >= 0 ? vol_drag_pct : audio_volume();
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", v);
    draw_text(fb, 24, top + 10, "Volume", COL_DIM, TEXT_PX_SMALL, FB_W - 120);
    int tw = text_width(buf, TEXT_PX_SMALL);
    draw_text(fb, FB_W - 24 - tw, top + 10, buf, COL_TEXT, TEXT_PX_SMALL, FB_W);

    int by = vol_bar_y(), bw = FB_W - 48;
    fill_rect(fb, 24, by, bw, 8, COL_LINE);
    int filled = bw * v / 100;
    fill_rect(fb, 24, by, filled, 8, COL_ACCENT);
    fill_circle(fb, 24 + filled, by + 4, 13, COL_ACCENT);
}

/* Just the progress bar and its two clocks. A scrub changes nothing else, and
 * repainting the whole screen — a 384x384 cover blit included — for a bar six
 * pixels tall is what made dragging feel heavy. */
static void draw_scrub_strip(uint16_t *fb) {
    if (cur_track < 0 || cur_track >= queue_n) return;
    lib_track_t *t = &queue[cur_track];

    int by = bar_y();
    int top = by - 16, h = 52;
    fill_rect(fb, 0, top, FB_W, h, COL_BG);

    int dur = audio_dur_ms();
    if (dur <= 0) dur = t->dur_ms;
    int pos = scrub_active ? scrub_ms(dur) : audio_pos_ms();

    int bh = scrub_active ? 10 : 6;
    int byy = by - (bh - 6) / 2;
    fill_rect(fb, 24, byy, FB_W - 48, bh, COL_LINE);
    if (dur > 0) {
        int w = (FB_W - 48) * pos / dur;
        if (w > FB_W - 48) w = FB_W - 48;
        fill_rect(fb, 24, byy, w, bh, COL_ACCENT);
        if (scrub_active) fill_circle(fb, 24 + w, by + 3, 13, COL_ACCENT);
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%d:%02d", pos / 60000, (pos / 1000) % 60);
    draw_text(fb, 24, by + 14, buf, COL_DIM, TEXT_PX_SMALL, FB_W);
    int rem = dur - pos;
    if (rem < 0) rem = 0;
    snprintf(buf, sizeof(buf), "-%d:%02d", rem / 60000, (rem / 1000) % 60);
    draw_right(fb, by + 14, buf);
}

/* R14 profiling, temporary. Accumulated in memory and reported once every
 * PROF_EVERY painted frames: mlog() writes to /usr/data, and BG27 established
 * that storage I/O on this thread is exactly what stalls the UI loop, so
 * logging per frame would measure the instrument rather than the redraw. */
#define PROF_EVERY 60
static int      prof_n;
static uint64_t prof_ui_us, prof_clear_us, prof_text_us;
static uint64_t prof_ui_max;
uint64_t g_prof_text_us;        /* added to by text.c's draw path */

static uint64_t us_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

uint64_t g_prof_clear_us;       /* added to by fill_rect's full-screen path */

static void draw_ui(uint16_t *fb) {
    uint64_t t0 = us_now();
    uint64_t clear0 = g_prof_clear_us, text0 = g_prof_text_us;

    draw_screen(fb);
    if (index_visible()) draw_index(fb);
    if (mini_visible()) draw_mini(fb);
    if (sheet_open) draw_sheet(fb);
    if (qs_open) draw_quick_settings(fb);
    else if (vol_ticks > 0) draw_volume(fb);
    if (edge_active) draw_back_hint(fb);

    uint64_t dt = us_now() - t0;
    prof_ui_us    += dt;
    prof_clear_us += g_prof_clear_us - clear0;
    prof_text_us  += g_prof_text_us - text0;
    if (dt > prof_ui_max) prof_ui_max = dt;
    if (++prof_n >= PROF_EVERY) {
        mlog("[prof] draw_ui avg %lu us (max %lu) | clear %lu | text %lu | other %lu\n",
             (unsigned long)(prof_ui_us / prof_n),
             (unsigned long)prof_ui_max,
             (unsigned long)(prof_clear_us / prof_n),
             (unsigned long)(prof_text_us / prof_n),
             (unsigned long)((prof_ui_us - prof_clear_us - prof_text_us) / prof_n));
        prof_n = 0; prof_ui_us = prof_clear_us = prof_text_us = 0; prof_ui_max = 0;
    }
}

/* ---- screen lock --------------------------------------------------------- */
/* The stock UI blanks the screen on its own timeout, but nothing does that
 * while this app owns the framebuffer and has the input nodes grabbed, so the
 * panel simply stayed lit forever.
 *
 * Power toggles the lock; it also locks itself after a spell of no input. The
 * media keys keep working while locked and deliberately do not wake the
 * screen — locked-with-music-playing is the pocket case, and lighting the
 * panel every time the volume changes defeats the point. Touch is drained and
 * discarded rather than left unread: events queued against our grab would
 * otherwise all replay the moment the screen came back, acting on whatever
 * screen happened to be up.
 */
#define BACKLIGHT "/sys/class/backlight/backlight_pwm0/brightness"
#define DEFAULT_BRIGHTNESS 43
/* The unit's blue notification LED, on by default (brightness 50 of 100)
 * from stock firmware startup with no obvious script setting it -- read
 * back from hiby_player's own binary, so it's written by the player itself
 * rather than a kernel/driver default, which is what makes a single
 * off-at-startup write not fully reliable on its own: if the stock code
 * reasserts it later, the write needs repeating, not just doing once. Tied
 * to button_locked specifically (the double-press lock), not the ordinary
 * screen lock -- that one fires on every idle timeout and a blinking LED
 * every time the screen naturally dims would be worse than the LED always
 * being on. */
#define LED_BLUE         "/sys/class/leds/blue/brightness"
#define LED_RED          "/sys/class/leds/red/brightness"
#define LED_BLUE_TRIGGER "/sys/class/leds/blue/trigger"
#define LED_RED_TRIGGER  "/sys/class/leds/red/trigger"
/* Not 100 (this LED's own max_brightness): same PWM-wraparound quirk as
 * BG17's backlight -- writing the literal max value wraps the duty cycle to
 * 0% instead of 100%, so the LED went fully dark instead of lighting up.
 * Confirmed live: sysfs read back "100" correctly after the write, so the
 * write itself wasn't the problem, only what value it was written with. */
#define LED_ON   99
/* This board's "breathing" kernel trigger is a no-op in practice -- selecting
 * it (confirmed via /sys/.../trigger showing [breathing]) produced a flat-on
 * LED, not a pulse, and it exposes none of the delay_on/delay_off files a
 * working software-blink trigger normally would. The pulse below is done by
 * hand instead: a plain triangle ramp written from the main loop, which
 * already ticks at a known, steady rate (see VOL_TICKS's own "~3s" comment
 * for the same 30Hz assumption). Both LEDs stay in manual (trigger=none) so
 * these raw brightness writes are what actually controls them. */
/* Counted in main-loop ticks, and the loop runs at 10 Hz while locked, which
 * is the only state this pulse ever runs in (it is gated on button_locked,
 * and that always locks). These were 150/300 back when every tick was 33 ms;
 * left alone they would have stretched the same pulse over fifteen seconds. */
#define LED_PULSE_TICKS 50     /* one full up/down cycle: ~5s */
#define LED_SWAP_TICKS  100    /* colour alternates every ~10s (2 pulses) */
/* Seconds of no input before the panel goes dark; 0 disables it. Off for now
 * by explicit request while the crash and index work settle — the auto-lock
 * was one of several things making it hard to tell what state a report was
 * actually describing. Meant to come back properly later (see the backlog),
 * not abandoned. */
#define CONF_PATH "/usr/data/music.conf"
#define IDLE_DEFAULT_S 0
static int idle_ticks = IDLE_DEFAULT_S * 30;    /* loop runs at ~30/s while awake */

static void load_conf(void) {
    FILE *f = fopen(CONF_PATH, "r");
    if (!f) return;
    /* Wide enough for eq_profile_path -- EP_PATH_LEN plus the key and '='. */
    char line[EP_PATH_LEN + 32];
    /* BG38: applied after the loop, once both are known, rather than as each
     * line is seen -- eq_on can appear before or after eq_profile_path, and
     * eq_switch_to() should only ever run once per load. -1 = not present in
     * the file (a config from before this existed), so eq_enabled() is left
     * at its compiled-in default rather than forced off. */
    char eq_path_buf[EP_PATH_LEN] = "";
    int eq_on_saved = -1;
    while (fgets(line, sizeof(line), f)) {
        int v;
        if (sscanf(line, "idle_lock_seconds = %d", &v) == 1 ||
            sscanf(line, "idle_lock_seconds=%d", &v) == 1) {
            if (v < 0) v = 0;
            idle_ticks = v * 30;
        } else if (sscanf(line, "accent_index = %d", &v) == 1 ||
                   sscanf(line, "accent_index=%d", &v) == 1) {
            if (v >= 0 && v < ACCENT_N) {
                g_accent_idx = v;
                g_accent = ACCENT_PRESETS[v].color;
            }
        } else if (sscanf(line, "button_lock_enabled = %d", &v) == 1 ||
                   sscanf(line, "button_lock_enabled=%d", &v) == 1) {
            button_lock_enabled = v != 0;
        } else if (sscanf(line, "deep_sleep = %d", &v) == 1 ||
                   sscanf(line, "deep_sleep=%d", &v) == 1) {
            /* Deliberately conf-only and off by default, with no Settings row
             * yet: this is the one feature here that can leave the device
             * unresponsive if the wake path misbehaves, and it has already done
             * so once. It earns a toggle in the UI after it has been shown to
             * suspend and wake reliably on real hardware, not before. */
            deep_sleep_enabled = v != 0;
        } else if (sscanf(line, "sleep_minutes = %d", &v) == 1 ||
                   sscanf(line, "sleep_minutes=%d", &v) == 1) {
            /* Stored as the value, not the index, so the file stays meaningful
             * on its own and reordering the choices cannot silently change
             * what an existing config means. */
            for (int i = 0; i < SLEEP_CHOICE_N; i++)
                if (SLEEP_CHOICES[i] == v) { sleep_idx = i; break; }
        } else if (sscanf(line, "auto_off_minutes = %d", &v) == 1 ||
                   sscanf(line, "auto_off_minutes=%d", &v) == 1) {
            for (int i = 0; i < AUTO_OFF_CHOICE_N; i++)
                if (AUTO_OFF_CHOICES[i] == v) { auto_off_idx = i; break; }
        } else if (sscanf(line, "eq_on = %d", &v) == 1 ||
                   sscanf(line, "eq_on=%d", &v) == 1) {
            eq_on_saved = v != 0;
        } else if (sscanf(line, "eq_profile_path = %383[^\r\n]", eq_path_buf) == 1 ||
                   sscanf(line, "eq_profile_path=%383[^\r\n]", eq_path_buf) == 1) {
            /* Paths can carry spaces (SD-card folder names do), hence %[^\r\n]
             * rather than %s, which would stop at the first one. */
        }
    }
    fclose(f);
    /* BG38: EQ settings did not survive a reboot -- nothing wrote them here
     * at all. eq_switch_to() is a no-op against a path that no longer exists
     * (a profile deleted or a card swapped since), same as any other stale
     * path this app already tolerates. */
    if (eq_path_buf[0]) eq_switch_to(eq_path_buf);
    if (eq_on_saved >= 0) eq_set_enabled(eq_on_saved);
    /* MSEB has its own file (MSEB_PATH), not a music.conf key -- there is
     * nothing profile-like about it to switch between, just one fixed set of
     * gains, so it does not need the eq_profile_path indirection above. */
    mseb_load(mseb_gain, &mseb_on);
    eq_set_mseb(mseb_on, mseb_gain);
    recent_heard_load();   /* R30 -- its own file, not a music.conf key either */
}

/* Does this line set `key`? Length taken from the key itself rather than
 * written out by hand: the hand-written version had "button_lock_enabled"
 * (19 characters) matched with a length of 20, so byte 20 compared the
 * literal's NUL against the line's space, never matched, and every save kept
 * the old line *and* appended a new one. The file had accumulated twenty-odd
 * copies before anyone looked -- which matters, because the reader below only
 * takes 64 lines and would eventually have pushed real settings out. */
static int conf_line_is(const char *line, const char *key) {
    return strncmp(line, key, strlen(key)) == 0;
}

/* Read-modify-write rather than a full rewrite from known keys: the file's
 * own comment block (see BG5) is meant to stay human-readable, and this
 * only ever touches the lines it owns. */
static void save_conf(void) {
    /* Wide enough for eq_profile_path -- see the matching line[] in load_conf(). */
    char lines[64][EP_PATH_LEN + 32];
    int n = 0;
    FILE *f = fopen(CONF_PATH, "r");
    if (f) {
        while (n < 64 && fgets(lines[n], sizeof(lines[0]), f)) {
            if (!conf_line_is(lines[n], "accent_index") &&
                !conf_line_is(lines[n], "button_lock_enabled") &&
                !conf_line_is(lines[n], "deep_sleep") &&
                !conf_line_is(lines[n], "sleep_minutes") &&
                !conf_line_is(lines[n], "auto_off_minutes") &&
                !conf_line_is(lines[n], "eq_on") &&
                !conf_line_is(lines[n], "eq_profile_path"))
                n++;
        }
        fclose(f);
    }
    f = fopen(CONF_PATH, "w");
    if (!f) return;
    for (int i = 0; i < n; i++) fputs(lines[i], f);
    fprintf(f, "accent_index = %d\n", g_accent_idx);
    fprintf(f, "button_lock_enabled = %d\n", button_lock_enabled);
    fprintf(f, "sleep_minutes = %d\n", sleep_minutes());
    fprintf(f, "deep_sleep = %d\n", deep_sleep_enabled);
    fprintf(f, "auto_off_minutes = %d\n", auto_off_minutes());
    /* BG38 */
    fprintf(f, "eq_on = %d\n", eq_enabled());
    if (eq_cur_path[0]) fprintf(f, "eq_profile_path = %s\n", eq_cur_path);
    fclose(f);
}

static int locked;
static int saved_brightness = -1;
static int g_fbfd = -1;

/* R20: a stronger lock than the plain screen lock above -- opt-in via
 * Settings, and once on, every hardware button is swallowed rather than
 * just the screen staying dark while media keys keep working. */
#define DOUBLE_PRESS_MS 400
static int button_locked;
/* The event's own kernel timestamp, not the time this loop got round to
 * reading it. Those are the same thing only while the loop runs flat out;
 * once it slows down when locked (which is the point -- see the sleep at the
 * bottom of the main loop), two presses 200 ms apart can be read one poll
 * apart and measure as however long that poll took, quietly putting a real
 * double press outside DOUBLE_PRESS_MS. Reading ev.time makes the window mean
 * what it says regardless of how often anyone looks. */
static struct timeval last_power_press;
static int have_last_power_press;

static int read_int_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    return v;
}

static void write_int_file(const char *path, int v) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%d", v);
    fclose(f);
}

static void write_text_file(const char *path, const char *s) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(s, f);
    fclose(f);
}

/* Advances only while button_locked (the caller checks); a plain triangle
 * ramp, colour swapping every LED_SWAP_TICKS. The every-third-tick throttle
 * that used to be here existed to get 30 Hz of loop down to 10 Hz of sysfs
 * writes; the loop itself now runs at 10 Hz whenever this is called, so the
 * write rate is already what it was and throttling again would only make the
 * ramp visibly steppy. */
static int led_tick;

static void led_pulse_step(void) {
    {
        int swap_to_red = (led_tick / LED_SWAP_TICKS) & 1;
        int p = led_tick % LED_PULSE_TICKS;
        int half = LED_PULSE_TICKS / 2;
        int level = p < half ? (p * LED_ON) / half
                              : ((LED_PULSE_TICKS - p) * LED_ON) / half;
        if (level < 0) level = 0;
        if (level > LED_ON) level = LED_ON;
        write_int_file(swap_to_red ? LED_RED : LED_BLUE, level);
        write_int_file(swap_to_red ? LED_BLUE : LED_RED, 0);
    }
    led_tick++;
}

/* ---- low-power idle ------------------------------------------------------ */
/* This used to drop the Wi-Fi association after sleep_minutes() of sitting
 * locked, and put it back on the way out. That is gone, for two reasons.
 *
 * The first is that it stopped being worth anything: deep_suspend() below
 * stops the SoC outright, which takes the radio with it, so switching Wi-Fi
 * off a few seconds beforehand saves a rounding error.
 *
 * The second is that it looked actively dangerous. Every lock and unlock ran
 * wifi_off.sh / wifi_on.sh, which kill wpa_supplicant and take the interface
 * down and up -- far more driver state-cycling than stock ever does, and
 * hiby_player polls the same driver throughout (there is a
 * `wpa_cli -i wlan0 signal_poll` child of it in the process list). After a
 * session of this the Broadcom driver wedged: dhd_dpc spinning at 97% of the
 * CPU in kernel space, load average 22, zero idle. Because dhd sits on the
 * SDIO bus the SD card shares, that starved storage too -- `sync` hung, an
 * exFAT mount took 492 seconds, and the device could not complete a reboot or
 * even `reboot -f`. It needed the battery pulling.
 *
 * Not proven to be the cause, but it is the one thing here doing something
 * stock does not, and the saving no longer justifies the suspicion. See BG27.
 */

/* ---- deep suspend -------------------------------------------------------- */
/* Suspend-to-RAM, which is the only thing left that actually stops the SoC and
 * the DDR rather than just idling them. Everything above is housekeeping by
 * comparison.
 *
 * The order below is not a guess. Writing `mem` cold was tried first and hangs
 * this device outright: the panel blanks, the backlight stays lit, and neither
 * the power button nor an RTC alarm armed and verified beforehand brings it
 * back, because the sequence never completes -- it cost two hard reboots to
 * establish. The working order was then recovered from open_hiby_player, a
 * third-party replacement player whose own implementation logs "returned from
 * mem sleep", i.e. actually resumes:
 *
 *     /usr/bin/bt_suspend  ->  blank fb via sysfs  ->  echo mem  ->  bt_resume
 *
 * bt_suspend is the part we were missing and is not optional: bluealsa,
 * bluetoothd and bt-agent all sit on the SDIO/UART the suspend path has to
 * quiesce, and the stock script even carries its own warning that the sleep(1)
 * inside it is load-bearing when called from C. The framebuffer is blanked
 * through /sys/class/graphics/fb0/blank rather than the FBIOBLANK ioctl,
 * because that is what the implementation known to work on this hardware
 * uses. Both scripts are byte-identical to stock 1.6 on this firmware. */
#define FB_BLANK_NODE  "/sys/class/graphics/fb0/blank"
#define PWR_STATE      "/sys/power/state"
#define RTC_WAKEALARM  "/sys/class/rtc/rtc0/wakealarm"
#define RTC_EPOCH      "/sys/class/rtc/rtc0/since_epoch"

/* Belt and braces, and cheap: if the power button ever fails to wake the
 * device the RTC brings it back rather than leaving it dead in a pocket, which
 * is exactly the failure this feature could otherwise produce. Cancelled on
 * the way out, so it costs nothing whenever the button works. */
#define SUSPEND_RTC_BACKSTOP_S 900

static int suspend_ok(void) {
    if (!deep_sleep_enabled) return 0;
    /* Playing is the only thing that blocks this, and the caller has already
     * excluded it. A *connected* headset used to block it too, on the grounds
     * that bt_suspend would drop the link -- but connected is not the same as
     * in use, the drop is recoverable (bt_resume rebuilds the stack and puts
     * the adapter back on), and waking by button is accepted. Left in, that
     * gate would have meant anyone who leaves a headset paired never suspends
     * at all, which is most of the saving gone for a reconnect nobody asked
     * to avoid. */
    return 1;
}

static void deep_suspend(void) {
    /* Stock's bt_resume ends with `bt-adapter --set "Powered" "Off"`, so a
     * round trip through suspend leaves Bluetooth switched off even if it was
     * on beforehand -- verified on device, hci0 reads DOWN afterwards. Note
     * what it was so it can be put back; anything else silently changes a
     * setting the user chose. */
    int bt_was_on = st_bt_on();
    int now = read_int_file(RTC_EPOCH);
    if (now > 0) {
        write_int_file(RTC_WAKEALARM, 0);                       /* clear any stale alarm */
        write_int_file(RTC_WAKEALARM, now + SUSPEND_RTC_BACKSTOP_S);
    }
    mlog("[music] suspend: bluetooth teardown\n");
    if (system("/usr/bin/bt_suspend >/dev/null 2>&1") == -1) { /* carry on regardless */ }
    write_int_file(FB_BLANK_NODE, 4);                           /* FB_BLANK_POWERDOWN */

    mlog("[music] suspend: entering mem sleep\n");
    write_text_file(PWR_STATE, "mem");                          /* blocks until resume */
    mlog("[music] suspend: resumed\n");

    write_int_file(RTC_WAKEALARM, 0);                           /* backstop no longer needed */
    /* Backgrounded: bt_resume re-runs brcm_patchram_plus with a sleep 5 and
     * several more after it, so it is ten seconds of work. Blocking the UI
     * thread for that would freeze the very double-press the user is about to
     * make to unlock. Same pattern st_wifi_set/st_bt_set already use.
     *
     * The power-on is chained onto it inside the same background shell rather
     * than issued here, because it has to happen *after* bt_resume's ten
     * seconds of stack rebuilding, not racing it. */
    if (system(bt_was_on
               ? "( /usr/bin/bt_resume; bt-adapter --set Powered On ) >/dev/null 2>&1 &"
               : "/usr/bin/bt_resume >/dev/null 2>&1 &") == -1) { }
}

static void set_locked(int on) {
    if (on == locked) return;
    locked = on;
    if (on) {
        saved_brightness = read_int_file(BACKLIGHT);
        write_int_file(BACKLIGHT, 0);
        /* Dark is not off. With only the backlight at zero the LCD controller
         * carries on scanning out: measured at ~61 framebuffer interrupts a
         * second while locked, which is 480*800*2 bytes pulled from DDR sixty
         * times a second -- around 46 MB/s of memory traffic to display a
         * black screen nobody is looking at, keeping the memory controller
         * busy the whole time the device is in a pocket. Powering the panel
         * down stops the scanout as well as the light.
         *
         * The matching unblank below already existed and is unconditional, so
         * the way back is the path that was always taken -- and the BG6
         * watchdog deliberately leaves a blanked panel alone while locked, so
         * it will not fight this. */
        if (g_fbfd >= 0) ioctl(g_fbfd, FBIOBLANK, FB_BLANK_POWERDOWN);
    } else {
        /* Unblank unconditionally. The player can power the framebuffer down
         * underneath us on its own display timeout, and restoring brightness
         * to a blanked panel leaves a black screen that no amount of pressing
         * will bring back. It costs nothing if it was never blanked. */
        if (g_fbfd >= 0) ioctl(g_fbfd, FBIOBLANK, FB_BLANK_UNBLANK);
        /* Through st_brightness_set(), not a direct write: saved_brightness
         * can be the literal max (the slider clamps to qs_bright_max, 101),
         * and writing that value directly goes dark instead of brightest --
         * st_brightness_set() is the one place that caps it to max-1. */
        st_brightness_set(saved_brightness > 0 ? saved_brightness : DEFAULT_BRIGHTNESS);
    }
    mlog("[music] %s\n", on ? "locked" : "unlocked");
}

/* ---- hardware keys ------------------------------------------------------- */
/* Two devices carry media keys and both are worth having:
 *
 *   event3  earpods_adc  — the inline remote: volume, play/pause, prev/next,
 *                          rewind and fast-forward
 *   event2  jz adc keyboard — the side buttons: volume, play/pause, prev
 *
 * Both are grabbed while the app is open so the stock player does not act on
 * the same press as well.
 */
#define KEY_VOLUMEDOWN_  114
#define KEY_VOLUMEUP_    115
#define KEY_NEXTSONG_    163
#define KEY_PLAYPAUSE_   164
#define KEY_PREVSONG_    165
#define KEY_REWIND_      168
#define KEY_FASTFWD_     208
#define KEY_POWER_       116
/* What BlueZ's AVRCP input device actually sends. It does not use PLAYPAUSE:
 * a headset's play and pause arrive as distinct PLAYCD and PAUSECD, which is
 * why the buttons appeared dead while the presses were reaching the log. */
#define KEY_PAUSE_       119
#define KEY_STOPCD_      166
#define KEY_PLAYCD_      200
#define KEY_PAUSECD_     201
#define KEY_PLAY_        207



/* The unit's own buttons do not report what their labels say: the button
 * marked skip-forward sends PLAYPAUSE and skip-back sends NEXTSONG. Mapping by
 * keycode alone therefore had forward pausing and back skipping forward. The
 * inline remote is a normal remote and does mean what it sends, so the two are
 * mapped separately rather than one being bent to fit the other.
 *
 * This unit has no play/pause button of its own; that control is on screen. */
typedef enum { KEYS_BUTTONS = 0, KEYS_REMOTE } key_src_t;

/* Returns non-zero if anything happened, so the caller can mark the UI dirty;
 * -1 means the fd itself has died and the caller should close and forget it
 * (see the comment below the loop). */
static int handle_keys(int fd, key_src_t src) {
    struct input_event ev;
    int acted = 0;
    ssize_t r;
    while (fd >= 0 && (r = read(fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
        if (ev.type != EV_KEY || ev.value == 0) continue;   /* presses only */
        mlog("[music] key %d\n", ev.code);
        /* Power is the lock key, and the only one that wakes the screen. A
         * double press (within DOUBLE_PRESS_MS) toggles the stronger
         * button lock instead, when that's enabled in Settings -- checked
         * before anything else so it still works while button_locked is
         * swallowing every other key below. */
        if (ev.code == KEY_POWER_) {
            long ms = have_last_power_press
                    ? (ev.time.tv_sec - last_power_press.tv_sec) * 1000L +
                      (ev.time.tv_usec - last_power_press.tv_usec) / 1000L
                    : -1;
            last_power_press = ev.time;
            have_last_power_press = 1;

            if (button_lock_enabled && ms >= 0 && ms < DOUBLE_PRESS_MS) {
                have_last_power_press = 0;   /* don't chain into a third press */
                button_locked = !button_locked;
                locked = !button_locked;     /* so set_locked() below isn't a same-state no-op */
                set_locked(button_locked);
                if (button_locked) {
                    led_tick = 0;            /* start each pulse cycle fresh, on blue */
                } else {
                    write_int_file(LED_BLUE, 0);
                    write_int_file(LED_RED, 0);
                }
                mlog("[music] button lock %s\n", button_locked ? "on" : "off");
                acted = 1; continue;
            }
            if (button_locked) { acted = 1; continue; }   /* single press: swallowed */

            /* set_locked(on) is a no-op when on == locked, which is right
             * for a double press but wrong here: the stock player can blank
             * the panel on its own display timeout without this app's
             * `locked` ever finding out, so the first press after that
             * happens sees locked==0 (still "awake" as far as this app
             * knows), toggles it to 1, and set_locked(1) on an already-dark
             * screen is invisible -- it took a second press to actually
             * wake it. Trust the real backlight value instead of the flag:
             * dark for any reason wakes it, in one press, regardless of
             * what locked currently believes. */
            int dark = read_int_file(BACKLIGHT) <= 0;
            if (dark) { locked = 1; set_locked(0); }
            else set_locked(1);
            acted = 1; continue;
        }

        /* Swallowed outright while button-locked -- but only this unit's own
         * buttons (BG20): the lock exists to stop a pocket from mashing the
         * touchscreen and the physical side buttons, not to cut off a
         * headset's remote, which is a deliberate action from a separate
         * object and the whole point of controlling playback without taking
         * the device out in the first place. */
        if (button_locked && src == KEYS_BUTTONS) { acted = 1; continue; }

        if (ev.code == KEY_VOLUMEUP_)   { audio_volume_step(+5); vol_ticks = VOL_TICKS; acted = 1; continue; }
        if (ev.code == KEY_VOLUMEDOWN_) { audio_volume_step(-5); vol_ticks = VOL_TICKS; acted = 1; continue; }

        if (src == KEYS_BUTTONS) {
            switch (ev.code) {
                case KEY_PLAYPAUSE_:            /* the skip-forward button */
                    if (audiobook_mode) ab_play_chapter(cur_track + 1);
                    else if (podcast_mode) audio_seek_ms(audio_pos_ms() + 30000);
                    else                play_index(cur_track + 1);
                    acted = 1; break;
                case KEY_NEXTSONG_:             /* the skip-back button */
                case KEY_PREVSONG_:
                    if (audiobook_mode) ab_play_chapter(cur_track - 1);
                    else if (podcast_mode) audio_seek_ms(audio_pos_ms() - 30000);
                    else if (audio_pos_ms() > 3000) audio_seek_ms(0);
                    else play_index(cur_track - 1);
                    acted = 1;
                    break;
                default: break;
            }
            continue;
        }

        switch (ev.code) {
            case KEY_PLAYPAUSE_:  audio_toggle();  acted = 1; break;
            /* Explicit play and pause, so a headset asking to pause cannot
             * toggle something already paused back into playing. */
            case KEY_PLAYCD_:
            case KEY_PLAY_:
                if (audio_is_paused()) audio_toggle();
                acted = 1;
                break;
            case KEY_PAUSECD_:
            case KEY_PAUSE_:
            case KEY_STOPCD_:
                if (!audio_is_paused() && audio_is_active()) audio_toggle();
                acted = 1;
                break;
            case KEY_NEXTSONG_:
                if (audiobook_mode) ab_play_chapter(cur_track + 1);
                else if (podcast_mode) audio_seek_ms(audio_pos_ms() + 30000);
                else                play_index(cur_track + 1);
                acted = 1;
                break;
            case KEY_PREVSONG_:
                /* Same rule as every other player: part-way in, previous means
                 * back to the start of this track — or of this chapter. A
                 * podcast episode instead gets the same +/-30s ad-skip as its
                 * on-screen transport (see the tap handler's comment). */
                if (audiobook_mode) {
                    const ab_chapter_t *ch =
                        (cur_track >= 0 && cur_track < ab_book.chap_n)
                            ? &ab_book.chap[cur_track] : NULL;
                    int64_t into = audio_pos_ms() - (ch ? ch->file_start_ms : 0);
                    if (into > 3000) audio_seek_ms((int)(ch ? ch->file_start_ms : 0));
                    else ab_play_chapter(cur_track - 1);
                } else if (podcast_mode) {
                    audio_seek_ms(audio_pos_ms() - 30000);
                } else if (audio_pos_ms() > 3000) {
                    audio_seek_ms(0);
                } else {
                    play_index(cur_track - 1);
                }
                acted = 1;
                break;
            case KEY_FASTFWD_: audio_seek_ms(audio_pos_ms() + 10000); acted = 1; break;
            case KEY_REWIND_:  audio_seek_ms(audio_pos_ms() - 10000); acted = 1; break;
            default: break;
        }
    }
    /* A short read of 0 is a true EOF, not "no data queued" (that's EAGAIN,
     * expected on nearly every poll of a nonblocking fd and not an error).
     * EOF means the device itself is gone: BlueZ tears down and recreates
     * the AVRCP input node on every headset reconnect, reusing the same
     * eventN path scan_inputs() already has recorded, and its "already have
     * this name" check has no way to tell a now-dead fd from a live one by
     * name alone -- so it never reopens it, and this app is left polling a
     * handle that will never produce another event, permanently, until the
     * whole app restarts. RBG1: headset controls worked once, then silently
     * stopped after the first reconnect. Signalling death here lets the
     * caller close it and forget the name so the next scan picks the fresh
     * device back up. */
    if (fd >= 0 && r == 0) return -1;
    if (fd >= 0 && r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return -1;
    return acted;
}

/* Open the fixed nodes plus anything that looks like a media remote. The
 * AVRCP device is a proper remote — its PLAYPAUSE means play/pause — unlike
 * this unit's own buttons, whose labels and keycodes disagree. */
static void scan_inputs(void) {
    static const struct { const char *node; int src; } fixed[] = {
        { "event0", KEYS_BUTTONS },   /* power, next */
        { "event2", KEYS_BUTTONS },   /* side buttons */
        { "event3", KEYS_REMOTE  },   /* inline remote */
    };
    for (unsigned i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        int have = 0;
        for (int k = 0; k < kfd_n; k++)
            if (!strcmp(kfd_name[k], fixed[i].node)) { have = 1; break; }
        if (have || kfd_n >= KFD_MAX) continue;
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/%s", fixed[i].node);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        ioctl(fd, EVIOCGRAB, 1);
        kfd[kfd_n] = fd;
        kfd_src[kfd_n] = fixed[i].src;
        snprintf(kfd_name[kfd_n], sizeof(kfd_name[0]), "%s", fixed[i].node);
        kfd_n++;
    }

    /* Anything named AVRCP, whichever event number it landed on. */
    FILE *f = fopen("/proc/bus/input/devices", "r");
    if (!f) return;
    char line[256], name[128] = "";
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "N: Name=", 8)) {
            snprintf(name, sizeof(name), "%s", line + 8);
        } else if (!strncmp(line, "H: Handlers=", 12) && strstr(name, "AVRCP")) {
            char *ev = strstr(line, "event");
            if (!ev) continue;
            char node[32];
            unsigned n = 0;
            while (n + 1 < sizeof(node) && ev[n] && ev[n] != ' ' && ev[n] != '\n') {
                node[n] = ev[n]; n++;
            }
            node[n] = '\0';
            int have = 0;
            for (int k = 0; k < kfd_n; k++)
                if (!strcmp(kfd_name[k], node)) { have = 1; break; }
            if (have || kfd_n >= KFD_MAX) continue;
            char path[48];
            snprintf(path, sizeof(path), "/dev/input/%s", node);
            int fd = open(path, O_RDONLY | O_NONBLOCK);
            if (fd < 0) continue;
            ioctl(fd, EVIOCGRAB, 1);
            kfd[kfd_n] = fd;
            kfd_src[kfd_n] = KEYS_REMOTE;
            snprintf(kfd_name[kfd_n], sizeof(kfd_name[0]), "%s", node);
            kfd_n++;
            mlog("[music] headset controls on %s\n", node);
        }
    }
    fclose(f);
}

/* ---- app -------------------------------------------------------------
 * Not `static`: the RP1-follow-on standalone build (standalone_main.c)
 * calls this directly from its own main(), bypassing the tile-hijack
 * trampoline entirely -- there is no launcher to hijack a tile from when
 * this binary is not running inside hiby_player. is_hiby_player()'s own
 * existing guard in music_init() already makes the constructor a safe
 * no-op under a different process name, so nothing else here needed to
 * change for that build to coexist with the normal LD_PRELOAD one. */
int music_entry(void *a0, void *a1) {
    (void)a0; (void)a1;
    mlog("[music] entering app\n");
    load_conf();
    screen = SC_MENU; reset_scroll();
    if (lib_open() != 0) mlog("[music] library open failed\n");

    int fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd < 0) { mlog("[music] no fb: %s\n", strerror(errno)); return 0; }
    g_fbfd = fbfd;                    /* set_locked needs it to unblank */

    struct fb_var_screeninfo v;
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &v) < 0) {
        mlog("[music] vinfo failed\n"); close(fbfd); return 0;
    }

    size_t page_px = (size_t)FB_W * FB_H;
    size_t map_len = page_px * 2 * 2;
    uint16_t *base = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (base == MAP_FAILED) {
        mlog("[music] mmap failed: %s\n", strerror(errno));
        close(fbfd); return 0;
    }

    /* Keep what the launcher had so it can be put back on the way out. */
    uint16_t *snapshot = malloc(page_px * 2);
    if (snapshot) memcpy(snapshot, base + (size_t)v.yoffset * FB_W, page_px * 2);

    int tfd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    if (tfd >= 0 && ioctl(tfd, EVIOCGRAB, 1) < 0)
        mlog("[music] touch grab failed: %s\n", strerror(errno));

    /* event0 is not optional: it carries the power key, and it is also where
     * NEXTSONG lives — the skip button did nothing until this was opened,
     * because event2 only reports volume, play/pause and previous.
     *
     * A Bluetooth headset's own buttons are a different matter again: BlueZ
     * creates an input device for them ("WH-1000XM4 (AVRCP)") only when the
     * headset connects, which is usually after this app started. So the list
     * is rescanned rather than opened once. */
    scan_inputs();
    mlog("[music] keys: %d device(s) open\n", kfd_n);

    int jack_was = st_headset();

    bt_poll_run = 1;
    bt_thread_valid = (pthread_create(&bt_thread, NULL, bt_poll, NULL) == 0);

    /* Redrawing every 33 ms regardless burns CPU on a screen that is usually
     * static, and this app exists partly because the stock one is not smooth.
     * A frame is produced only when something actually changed: a gesture, the
     * clock ticking over a second, or artwork arriving. */
    int page = 0, frames = 0, running = 1, dirty = 1;
    int last_sec = -1, art_seen = 0, status_tick = 0, idle = 0, rescan_tick = 0;
    int sleep_idle = 0, auto_off_idle = 0;
    int ab_pos_tick = 0;
    int blank_tick = 0, last_lit_bright = 0;    /* BG6 watchdog, see below */
    while (running) {
        g_tick++;
        int x, y;
        int g = (tfd >= 0) ? read_gesture(tfd, &x, &y) : 0;
        if (locked) g = 0;             /* drained above, acted on here: never */
        else if (g) idle = 0;

        /* BG38 (part 2): drain bt_poll's fuzzy-matched profile, if it found
         * one since the last time round. The switch itself has to happen
         * here, not on that thread -- see the comment on bt_match_profile(). */
        {
            char eqpath[EP_PATH_LEN];
            pthread_mutex_lock(&bt_lock);
            snprintf(eqpath, sizeof(eqpath), "%s", bt_eq_pending_path);
            bt_eq_pending_path[0] = '\0';
            pthread_mutex_unlock(&bt_lock);
            if (eqpath[0] && strcmp(eqpath, eq_cur_path) != 0) {
                eq_switch_to(eqpath);
                save_conf();
                dirty = 1;
            }
        }

        /* A track that reaches its end should roll into the next one; stopping
         * dead at every track boundary is the one thing a music player cannot
         * do. Only a natural end counts — audio_stop clears was_active. */
        /* The worker moves itself on at a track boundary now, so the UI's job
         * is to follow: update what is showing, fetch the new artwork, and
         * hand over the track after that. */
        {
            /* Written first as `while (adv-- > 0) ...; if (adv >= 0)`, which
             * leaves adv at -1 whatever it started as, so the follower was
             * never queued here — only in play_index. Every other boundary
             * therefore fell back to a full restart and gapped. */
            int adv = audio_take_advance();
            if (adv > 0 && audiobook_mode) {
                /* A book rolls over a *file*, which may be several chapters
                 * further on — the chapter within it is then found by
                 * ab_follow() from the position, same as any other crossing. */
                for (int i = 0; i < adv; i++) {
                    int f = ab_book.chap[cur_track].file;
                    int j = cur_track;
                    while (j < ab_book.chap_n && ab_book.chap[j].file == f) j++;
                    if (j >= ab_book.chap_n) break;
                    cur_track = j;
                    snprintf(ab_playing, sizeof(ab_playing), "%s",
                             ab_book.files[ab_book.chap[j].file]);
                    art_request(ab_playing);
                    mlog("[music] rolled into %s\n", ab_book.chap[j].title);
                    dirty = 1;
                }
                audio_set_next(ab_next_file(cur_track));
            } else if (adv > 0) {
                for (int i = 0; i < adv && cur_track + 1 < queue_n; i++) {
                    cur_track++;
                    art_request(queue[cur_track].path);
                    mlog("[music] rolled into %s\n", queue[cur_track].name);
                    dirty = 1;
                }
                queue_follower();
            }
            if (ab_follow()) dirty = 1;
        }

        /* Fallback for the case the worker could not continue — a missing or
         * unreadable file, say. A stream that stops has simply dropped out,
         * and jumping into the music library is not what anyone listening to
         * the radio wants. */
        if (was_active && !audio_is_active()) {
            was_active = 0;
            if (audiobook_mode) {
                /* Only a real file boundary counts. Running off the end of a
                 * chapter inside a file is not the end of anything — the
                 * decoder plays straight on and ab_follow() moves the display
                 * — so the only thing to pick up here is the next file. */
                int f = (cur_track >= 0 && cur_track < ab_book.chap_n)
                      ? ab_book.chap[cur_track].file : -1;
                int j = cur_track;
                while (j < ab_book.chap_n && ab_book.chap[j].file == f) j++;
                if (f >= 0 && j < ab_book.chap_n) {
                    mlog("[music] fell back to restart at chapter %d\n", j + 1);
                    ab_play_chapter(j);
                }
            } else if (!radio_mode && cur_track + 1 < queue_n) {
                /* Reaching here means the worker could not roll on by itself,
                 * which costs a restart and therefore a gap. Logged, because
                 * a gap is otherwise only findable by ear. */
                mlog("[music] fell back to restart at track %d\n", cur_track + 1);
                play_index(cur_track + 1);
            } else if (podcast_mode && cur_track >= 0 && cur_track < queue_n) {
                /* An episode is never queued alongside a next one (see
                 * podcast_mode's comment), so reaching the end just stops --
                 * marked finished right away rather than left for the next
                 * periodic save, so reopening it immediately after shows it
                 * as done instead of resuming one tick's worth from the end.
                 * Not pod_save_current_pos(): audio_is_active() is already
                 * false here (that's what this whole block is gated on), and
                 * pod_save_current_pos() deliberately no-ops in that state --
                 * same reason ab_save_current_pos() has to be called before a
                 * stop, not after. */
                pod_resume_store(queue[cur_track].path, POD_FINISHED, 0);
            }
        }
        /* Also opened on a completed drag, not only by following the finger
         * down. A quick flick can be delivered entirely within one poll, and
         * then the live tracking never sees a finger that is still down. */
        if (!qs_open && g == 2 && touch_y < QS_PULL_ZONE &&
            !(vol_ticks > 0 && touch_y >= STATUS_H) && y > QS_PULL) {
            qs_open = 1;
            qs_refresh();
            dirty = 1; idle = 0;
        } else if (vol_ticks > 0 && !qs_open && g == 1 &&
                   y >= STATUS_H && y < STATUS_H + VOL_H) {
            int bw = FB_W - 48;
            int v = (x - 24) * 100 / (bw > 0 ? bw : 1);
            audio_volume_set(v < 0 ? 0 : (v > 100 ? 100 : v));
            vol_ticks = VOL_TICKS;
            dirty = 1; idle = 0;
        } else if (qs_open && (g == 1 || g == 2)) {
            if (qs_dragging) {
                /* The live block already applied the value continuously
                 * while the finger was down (qs_open && qs_dragging &&
                 * touch_down, below); this is just the release. Left to
                 * reach the branches below, a brightness drag that picked
                 * up enough incidental vertical wobble near either end to
                 * classify as g==2 on release would hit "swiped back up"
                 * and silently close the panel, or as g==1 land on
                 * whatever row the release Y happened to fall on — either
                 * way with the *release* position, not where the finger
                 * actually was on the bar. This is BG1 ("brightness slider
                 * overshoot turns screen off"): closing the panel while
                 * mid-drag through an otherwise-unrelated y<0 branch reads
                 * exactly like that from the outside. qs_dragging still
                 * reads true here because it isn't cleared until the live
                 * block runs later this same tick. */
            } else if (g == 2 && y < 0) {            /* swiped back up */
                qs_open = 0;
            } else if (g == 1) {
                int by = qs_bar_y();
                if (y > by - 26 && y < by + 26) {
                    int bw = FB_W - 48;
                    int v = (x - 24) * qs_bright_max / (bw > 0 ? bw : 1);
                    qs_bright = v < 1 ? 1 : (v > qs_bright_max ? qs_bright_max : v);
                    st_brightness_set(qs_bright);
                    saved_brightness = qs_bright;
                } else if (y > qs_wifi_y() && y < qs_wifi_y() + QS_ROW_H) {
                    qs_wifi = !qs_wifi;
                    st_wifi_set(qs_wifi);
                } else if (y > qs_bt_y() && y < qs_bt_y() + QS_ROW_H) {
                    qs_bt = !qs_bt;
                    st_bt_set(qs_bt);
                } else if (y > qs_eq_y() && y < qs_eq_y() + QS_ROW_H) {
                    if (x > FB_W - 100) {
                        eq_set_enabled(!eq_enabled());
                        save_conf();          /* BG38 */
                    } else {
                        /* Closed first, not left open behind the sheet --
                         * qs_open && g==1 is checked ahead of sheet_open in
                         * this chain, so a sheet row tap would otherwise be
                         * swallowed here instead of reaching the sheet. */
                        qs_open = 0;
                        eq_profile_n = ep_scan(eq_profiles, EP_MAX_PROFILES);
                        sheet_open = 3;
                    }
                } else if (y > qs_mseb_y() && y < qs_mseb_y() + QS_ROW_H) {
                    /* No sheet to open for this one -- there's nothing to
                     * pick, just the one fixed set of bands -- so the whole
                     * row toggles, not just the switch end of it. */
                    mseb_on = !mseb_on;
                    eq_set_mseb(mseb_on, mseb_gain);
                    mseb_save(mseb_gain, mseb_on);
                } else if (y > QS_H) {
                    qs_open = 0;                    /* tapped away */
                }
            }
            dirty = 1; idle = 0;
        } else if (g == 3) {
            /* Swipe in from the left edge: the way out of the player, which
             * otherwise only offered the queue. go_back() itself closes show
             * notes first when they're open, rather than leaving the player
             * under them -- see its own comment. */
            if (!go_back()) running = 0;
            dirty = 1; idle = 0;
        } else if (index_visible() && touch_x >= FB_W - INDEX_TOUCH_W &&
                   touch_y >= CONTENT_Y && touch_y < index_bottom()) {
            /* Anything that began on the strip ends on the strip. Sliding
             * left and lifting used to arrive here as a tap at the release
             * point, which is over a list row, and opened it.
             *
             * touch_y, not live_y, is the bug BG3/BG4 turned out to be:
             * touch_y is frozen at wherever the finger first touched down
             * and never updates during the drag, so this re-jump undid the
             * entire live-tracked drag on release — dragging from M up to J
             * looked right the whole time and then snapped back to M, and a
             * subdivision selected mid-drag reverted the instant the finger
             * lifted, which is what "the second level is gone" actually was. */
            if (index_shown >= 0) index_jump(live_y);
        } else if (g == 2 && index_visible() && touch_x >= FB_W - INDEX_TOUCH_W) {
            /* Already followed live; nothing to do on release. */
        } else if (g == 2) {
            /* Already followed live below, one row at a time, with inertia
             * picking up from here on release. This used to jump by however
             * many whole rows the release distance divided into — four at a
             * time on an ordinary swipe, and nothing moved until the finger
             * lifted. */
        } else if (g == 1 && sheet_open) {
            int top = sheet_top();
            int i = (y >= top + SHEET_HEAD) ? (y - top - SHEET_HEAD) / SHEET_ROW : -1;
            if (sheet_open == 2) {
                if (i >= 0 && i < playlist_n && sheet_track < track_n) {
                    int rc = pl_append(playlists[i].path, tracks[sheet_track].path);
                    snprintf(sheet_note, sizeof(sheet_note), "%s %s",
                             rc == 1 ? "Added to" : "Already in", playlists[i].name);
                }
                sheet_open = 0;
            } else if (sheet_open == 3) {
                /* No save-on-the-way-out here either, for the same reason as
                 * go_back()'s SC_EQ_BAND case: switching profiles without
                 * ever touching a slider must not fork the outgoing one. */
                if (i >= 0 && i < eq_profile_n) {
                    eq_switch_to(eq_profiles[i].path);
                    save_conf();          /* BG38 */
                }
                sheet_open = 0;
            } else if (i == 0) {
                queue_insert(sheet_track, cur_track + 1);
                snprintf(sheet_note, sizeof(sheet_note), "Playing next");
                sheet_open = 0;
            } else if (i == 1) {
                queue_insert(sheet_track, -1);
                snprintf(sheet_note, sizeof(sheet_note), "Added to queue");
                sheet_open = 0;
            } else if (i == 2) {
                playlist_n = pl_list(playlists, PL_MAX);
                sheet_open = 2;                 /* second level: which playlist */
            } else {
                sheet_open = 0;
            }
            dirty = 1; idle = 0;
        } else if (g == 1) {
            sheet_note[0] = '\0';
            if (screen == SC_PLAYING && audiobook_mode && y >= STATUS_H) {
                /* Same corner, same meaning as the regular player: the
                 * queue is chapters here, reached the same way. */
                if (y > FB_H - 56 && x > FB_W - 76) { go_back(); }
                else {
                    /* Book bar: display-only, per explicit correction --
                     * no hit test here at all, on purpose. */
                    int cby = ab_chapter_bar_y();
                    if (y > cby - 26 && y < cby + 26) {
                        const ab_chapter_t *ch =
                            (cur_track >= 0 && cur_track < ab_book.chap_n)
                                ? &ab_book.chap[cur_track] : NULL;
                        int dur = ch ? (int)ch->dur_ms : 0;
                        if (dur <= 0) dur = audio_dur_ms();
                        int px = x - 24, span = FB_W - 48;
                        if (px < 0) px = 0;
                        if (px > span) px = span;
                        /* Scrubbing is within the chapter, so the target is
                         * measured from where the chapter starts in the file
                         * rather than from the start of an eleven-hour file. */
                        if (dur > 0)
                            audio_seek_ms((int)((ch ? ch->file_start_ms : 0) +
                                                (int64_t)dur * px / span));
                    }

                    int cyy = cby + 58;
                    int mid = FB_W / 2;
                    /* Must match the skip rings' `off` in the draw code above
                     * (currently 96) -- this was left at the ring's old
                     * offset (128) after the draw side was pulled in, so the
                     * hit zone sat 32px away from where the speed ring is
                     * actually drawn. */
                    int scx = mid - 96 - 70;
                    if (y > cyy - 48 && y < cyy + 48) {
                        if (x > scx - 30 && x < scx + 30) {
                            ab_speed_permille += 100;
                            if (ab_speed_permille > 2000) ab_speed_permille = 1000;
                            audio_set_speed(ab_speed_permille);
                        } else if (x < FB_W / 3) {
                            audio_seek_ms(audio_pos_ms() - 10000);
                        } else if (x > 2 * FB_W / 3) {
                            audio_seek_ms(audio_pos_ms() + 10000);
                        } else {
                            audio_toggle();
                        }
                    }
                }
            } else if (screen == SC_PLAYING && radio_mode && y >= STATUS_H) {
                int cyy = 120 + 190;
                if (y > cyy - 48 && y < cyy + 48) audio_toggle();
            } else if (screen == SC_PLAYING && y >= STATUS_H) {
                /* The queue control sits in the corner the header used to own. */
                if (y > FB_H - 56 && x > FB_W - 76) { go_back(); }
                else {
                /* The bar is only 6 px tall; the target has to be the band
                 * around it or it is unhittable with a finger. */
                int bary = bar_y();
                if (y > bary - 26 && y < bary + 26) {
                    int dur = audio_dur_ms();
                    if (dur <= 0 && cur_track < queue_n) dur = queue[cur_track].dur_ms;
                    int px = x - 24, span = FB_W - 48;
                    if (px < 0) px = 0;
                    if (px > span) px = span;
                    if (dur > 0) audio_seek_ms((int)((int64_t)dur * px / span));
                }

                int cyy = bary + 70;      /* BG40: matches the draw-side offset */
                if (y > cyy - 48 && y < cyy + 48) {
                    /* BG47 (revised): real hit zones under the drawn arcs/
                     * ring, not blind thirds -- boundaries are the
                     * midpoints between adjacent element centres, same
                     * POD_SKIP_OFF/pod_speed_x() the draw side uses so the
                     * two cannot drift apart. Just two skip zones now
                     * (-10s, +30s), not the original four. A podcast
                     * episode is never queued alongside a next one (see
                     * podcast_mode's comment), so this replaces prev/next
                     * track entirely rather than sharing the zone with it. */
                    if (podcast_mode) {
                        int mid = FB_W / 2;
                        int x10 = mid - POD_SKIP_OFF, xp30 = mid + POD_SKIP_OFF;
                        int xspd = pod_speed_x(mid);
                        int xicon = pod_info_x(mid);
                        if (x < (xspd + x10) / 2) {
                            pod_speed_permille += 100;
                            if (pod_speed_permille > 2000) pod_speed_permille = 1000;
                            audio_set_speed(pod_speed_permille);
                        } else if (x < (x10 + mid) / 2) {
                            audio_seek_ms(audio_pos_ms() - 10000);
                        } else if (x < (mid + xp30) / 2) {
                            audio_toggle();
                        } else if (!pod_notes_avail || x < (xp30 + xicon) / 2) {
                            audio_seek_ms(audio_pos_ms() + 30000);
                        } else {
                            pod_notes_showing = !pod_notes_showing;
                            pod_notes_scroll_px = 0;
                        }
                    } else {
                        if (x < FB_W / 3)            play_index(cur_track - 1);
                        else if (x > 2 * FB_W / 3)   play_index(cur_track + 1);
                        else                          audio_toggle();
                    }
                }
                }
            } else if (y < CONTENT_Y) {
                /* mseb_reset_x()/pod_sync_x()'s own on-screen position sits
                 * well left of the old fixed "FB_W - 120" boundary these
                 * zones used to end at, which left a dead strip between the
                 * end of "Reset"/"Sync" and where that boundary began --
                 * landing there fell through to the BACK-zone check below
                 * and quietly left the screen instead of hitting the button.
                 * header_back_x() - 16 is precise: it reaches right up to
                 * where BACK's own zone actually starts. */
                if (screen == SC_MSEB && x >= mseb_reset_x() - 16 && x < header_back_x() - 16) {
                    for (int i = 0; i < MSEB_BAND_N; i++) mseb_gain[i] = 0.0f;
                    eq_set_mseb(mseb_on, mseb_gain);
                    mseb_save(mseb_gain, mseb_on);
                } else if (screen == SC_PODCASTS && x >= pod_sync_x() - 16 && x < header_back_x() - 16) {
                    if (!pod_update_running()) { pod_update_start(); pod_sync_log_n = 0; }
                    screen = SC_POD_SYNC; reset_scroll();
                } else if (x > FB_W - 120) go_back();
            } else if (index_visible() && x >= FB_W - INDEX_TOUCH_W && y >= CONTENT_Y &&
                       y < index_bottom()) {
                index_jump(y);
            } else if (mini_visible() && y >= FB_H - MINI_H) {
                /* Zone boundaries, not per-button hit tests -- generous on
                 * purpose, same reasoning as the full audiobook player's own
                 * thirds-based skip zones, and it sidesteps BG21 entirely:
                 * there's no separate copy of a button's position to drift
                 * out of sync with, only where the NEXT one over begins. */
                if (radio_mode) {
                    if (x > FB_W - 92) audio_toggle();
                    else               screen = SC_PLAYING;
                } else if (audiobook_mode) {
                    if (x > MINI_ZONE_SIDE)      audio_seek_ms(audio_pos_ms() + 10000);
                    else if (x > MINI_ZONE_PLAY) audio_toggle();
                    else if (x > MINI_ZONE_BACK) audio_seek_ms(audio_pos_ms() - 10000);
                    else                          screen = SC_PLAYING;
                } else if (podcast_mode) {
                    if (x > MINI_ZONE_SIDE)      audio_seek_ms(audio_pos_ms() + 30000);
                    else if (x > MINI_ZONE_PLAY) audio_toggle();
                    else if (x > MINI_ZONE_BACK) audio_seek_ms(audio_pos_ms() - 30000);
                    else                          screen = SC_PLAYING;
                } else {
                    if (x > MINI_ZONE_SIDE)      play_index(cur_track + 1);
                    else if (x > MINI_ZONE_PLAY) audio_toggle();
                    else { screen = SC_PLAYING; played_from_browse = 0; }
                }
            } else if (screen == SC_EQ) {
                int ry_enabled = eq_row_enabled_y();
                int ry_profile = eq_row_profile_y();
                int ry_bands   = eq_row_bands_y();
                if (y >= ry_enabled && y < ry_enabled + ROW_H) {
                    eq_set_enabled(!eq_enabled());
                    save_conf();          /* BG38 */
                } else if (y >= ry_profile && y < ry_profile + ROW_H) {
                    eq_profile_n = ep_scan(eq_profiles, EP_MAX_PROFILES);
                    sheet_open = 3;
                } else if (y >= ry_bands && y < ry_bands + ROW_H) {
                    screen = SC_EQ_BANDS; reset_scroll();
                    total = eq_cur.band_n;
                } else {
                    int pby = eq_preamp_y() + 26;
                    if (y > pby - 20 && y < pby + 20) {
                        int span = FB_W - 48, px = x - 24;
                        if (px < 0) px = 0; if (px > span) px = span;
                        eq_cur.preamp_db = -12.0f + 18.0f * (float)px / (float)span;
                        eq_set_preamp(eq_cur.preamp_db);
                        eq_save_current();
                    }
                }
            } else if (screen == SC_MSEB) {
                int ry_enabled = mseb_row_enabled_y();
                if (y >= ry_enabled && y < ry_enabled + ROW_H) {
                    mseb_on = !mseb_on;
                    eq_set_mseb(mseb_on, mseb_gain);
                    mseb_save(mseb_gain, mseb_on);
                } else {
                    int span = FB_W - 48;
                    for (int i = 0; i < MSEB_BAND_N; i++) {
                        int sy = mseb_band_row_y(i) + 46;
                        if (y <= sy - 20 || y >= sy + 20) continue;
                        int px = x - 24; if (px < 0) px = 0; if (px > span) px = span;
                        mseb_gain[i] = -12.0f + 24.0f * (float)px / (float)span;
                        eq_set_mseb(mseb_on, mseb_gain);
                        mseb_save(mseb_gain, mseb_on);
                        break;
                    }
                }
            } else if (screen == SC_EQ_BAND) {
                eq_band_t *b = (eq_editing_band >= 0 && eq_editing_band < eq_cur.band_n)
                             ? &eq_cur.band[eq_editing_band] : NULL;
                if (b) {
                    int ey = eq_enabled_y();
                    int ty = eq_type_y();
                    int fy = eq_freq_y(), gy = eq_gain_y(), qy = eq_q_y();
                    int span = FB_W - 48;
                    if (y > ey - 12 && y < ey + 20) {
                        b->on = !b->on;
                        eq_set_band(eq_editing_band, b);
                        eq_save_current();
                    } else if (y >= ty && y < ty + 34) {
                        int segw = span / 3;
                        int s = (x - 24) / (segw > 0 ? segw : 1);
                        if (s < 0) s = 0; if (s > 2) s = 2;
                        b->type = (eq_type_t)s;
                        eq_set_band(eq_editing_band, b);
                        eq_save_current();
                    } else if (y > fy - 20 && y < fy + 20) {
                        int px = x - 24; if (px < 0) px = 0; if (px > span) px = span;
                        b->fc = eq_freq_from_t((float)px / (float)span);
                        eq_set_band(eq_editing_band, b);
                        eq_save_current();
                    } else if (y > gy - 20 && y < gy + 20) {
                        int px = x - 24; if (px < 0) px = 0; if (px > span) px = span;
                        b->gain_db = -12.0f + 24.0f * (float)px / (float)span;
                        eq_set_band(eq_editing_band, b);
                        eq_save_current();
                    } else if (y > qy - 20 && y < qy + 20) {
                        int px = x - 24; if (px < 0) px = 0; if (px > span) px = span;
                        b->q = 0.1f + 9.9f * (float)px / (float)span;
                        eq_set_band(eq_editing_band, b);
                        eq_save_current();
                    }
                }
            } else if (screen == SC_SETTINGS) {
                int ry_lock = set_row_lock_y(), ry_theme = set_row_theme_y();
                int ry_autooff = set_row_autooff_y(), ry_about = set_row_about_y();
                if (y >= ry_lock && y < ry_lock + ROW_H) {
                    button_lock_enabled = !button_lock_enabled;
                    save_conf();
                } else if (y >= ry_autooff && y < ry_autooff + ROW_H) {
                    /* Cycles rather than opening a picker: six short values,
                     * and a whole screen for them would be more chrome than
                     * the choice is worth. */
                    auto_off_idx = (auto_off_idx + 1) % AUTO_OFF_CHOICE_N;
                    save_conf();
                } else if (y >= ry_theme && y < ry_theme + ROW_H) {
                    screen = SC_SETTINGS_THEME; reset_scroll();
                } else if (y >= ry_about && y < ry_about + ROW_H) {
                    screen = SC_SETTINGS_ABOUT; reset_scroll();
                }
            } else if (screen == SC_SETTINGS_THEME) {
                int idx = (y - CONTENT_Y) / ROW_H;
                if (idx >= 0 && idx < ACCENT_N) {
                    g_accent_idx = idx;
                    g_accent = ACCENT_PRESETS[idx].color;
                    save_conf();
                }
            } else {
                /* +scroll_px: the row under a tap follows the same visual
                 * offset the drag applied when the list is not at a row
                 * boundary. Always 0 on SC_MENU, which never scrolls. */
                int idx = (y - CONTENT_Y + scroll_px) / ROW_H;
                if (screen == SC_MENU) {
                    if (idx >= TOP_N) { /* nothing there */ }
                    else if (idx == TOP_MUSIC) {
                        screen = SC_MUSIC_MENU; reset_scroll();
                    } else if (idx == TOP_AUDIOBOOKS) {
                        ab_book_n = ab_scan_books(ab_books, AB_MAX_BOOKS);
                        ab_rebuild_rows();
                        screen = SC_AUDIOBOOKS; reset_scroll();
                        total = ab_book_n;
                        mlog("[music] %d audiobooks\n", ab_book_n);
                    } else if (idx == TOP_PODCASTS) {
                        pod_feed_n = pod_scan_feeds(pod_feeds, POD_MAX_FEEDS);
                        pod_rebuild_rows();
                        screen = SC_PODCASTS; reset_scroll();
                        total = pod_feed_n;
                        mlog("[music] %d podcast feeds\n", pod_feed_n);
                    } else if (idx == TOP_EQ) {
                        eq_profile_n = ep_scan(eq_profiles, EP_MAX_PROFILES);
                        /* First visit this session, nothing chosen yet: load
                         * whatever sorts first rather than show an empty
                         * screen when a profile is sitting right there. */
                        if (!eq_cur_path[0] && eq_profile_n > 0) {
                            eq_switch_to(eq_profiles[0].path);
                            save_conf();          /* BG38 */
                        }
                        screen = SC_EQ; reset_scroll();
                        mlog("[music] %d EQ profiles\n", eq_profile_n);
                    } else if (idx == TOP_MSEB) {
                        screen = SC_MSEB; reset_scroll();
                    } else if (idx == TOP_RADIO) {
                        station_n = radio_load(stations, RADIO_MAX);
                        screen = SC_RADIO; reset_scroll();
                        mlog("[music] %d stations\n", station_n);
                    } else if (idx == TOP_SETTINGS) {
                        screen = SC_SETTINGS; reset_scroll();
                    }
                } else if (screen == SC_MUSIC_MENU) {
                    if (idx >= MENU_N) { /* nothing there */ }
                    else if (idx == MENU_PLAYLISTS) {
                        recent_mode = 0;
                        playlist_n = pl_list(playlists, PL_MAX);
                        screen = SC_PLAYLISTS; reset_scroll();
                        mlog("[music] %d playlists\n", playlist_n);
                    } else if (idx == MENU_RECENT_ADDED || idx == MENU_RECENT_HEARD) {
                        /* Not lib_albums()/load_page() -- this list is a
                         * one-shot recency ranking, not something a facet or
                         * an A-Z offset can page through, so it is computed
                         * once here and handed to the same rows[]/row_at()
                         * the normal album list already uses to draw and to
                         * turn a tap into SC_TRACKS. */
                        recent_mode = (idx == MENU_RECENT_ADDED) ? RECENT_ADDED : RECENT_HEARD;
                        cur_facet = NULL;
                        cur_facet_label = menu[idx].label;
                        cur_artist[0] = '\0';
                        albums_artist[0] = '\0';
                        screen = SC_ALBUMS; reset_scroll();
                        row_n = (recent_mode == RECENT_ADDED)
                              ? lib_albums_recent_added(rows, RECENT_ALBUMS_N)
                              : lib_albums_recent_heard(recent_heard_ts, rows, RECENT_ALBUMS_N);
                        row_base = 0;
                        total = row_n;
                        mlog("[music] %s: %d\n", cur_facet_label, total);
                    } else if (menu[idx].column) {
                        recent_mode = 0;
                        cur_facet = menu[idx].column;
                        cur_facet_label = menu[idx].label;
                        screen = SC_ARTISTS; reset_scroll();
                        total = lib_group_count(cur_facet);
                        load_page();
                        mlog("[music] %s: %d\n", cur_facet_label, total);
                    } else {
                        /* Albums lists the whole library, with no facet above
                         * it to go back to. */
                        recent_mode = 0;
                        cur_facet = NULL;
                        cur_facet_label = menu[idx].label;
                        cur_artist[0] = '\0';
                        albums_artist[0] = '\0';
                        screen = SC_ALBUMS; reset_scroll();
                        total = lib_albums_count(NULL, NULL);
                        load_page();
                        mlog("[music] all albums: %d\n", total);
                    }
                } else if (screen == SC_ARTISTS && row_at(scroll + idx)) {
                    lib_row_t *row = row_at(scroll + idx);
                    /* BG15: row->owner is lib_group()'s marker for "this row's
                     * name is the 'Unknown' display label, not real data" —
                     * filter by the sentinel instead so the empty/NUL column
                     * is matched rather than the literal text "Unknown". */
                    snprintf(cur_artist, sizeof(cur_artist), "%s",
                             row->owner[0] ? LIB_UNKNOWN_MARK : row->name);
                    snprintf(albums_artist, sizeof(albums_artist), "%s", cur_artist);
                    artists_scroll_saved = scroll;         /* BG37 */
                    artists_scroll_px_saved = scroll_px;
                    screen = SC_ALBUMS; reset_scroll();
                    total = row->count;
                    load_page();
                    mlog("[music] %s -> %d albums\n", cur_artist, total);
                } else if (screen == SC_TRACKS && scroll + idx < track_n) {
                    if (pod_list) {
                        /* This list is one feed's episodes. An undownloaded
                         * one starts its download instead of playing --
                         * nothing to play yet -- one at a time, same as the
                         * whole-feed sync below. */
                        int pi = scroll + idx;
                        if (pod_eps[pi].downloaded) {
                            audio_set_speed(1000);
                            screen = SC_PLAYING;
                            pod_play_episode(pi);
                        } else if (!pod_download_active()) {
                            pod_download_start(pi);
                        }
                    } else if (ab_list) {
                        /* This list is the book's chapters. */
                        screen = SC_PLAYING;
                        ab_play_chapter(scroll + idx);
                    } else {
                        /* A regular track. Clear the mode and speed
                         * explicitly rather than leaving them wherever an
                         * earlier book left them, or a book's 1.3x would
                         * carry into a song. */
                        audio_set_speed(1000);
                        screen = SC_PLAYING;
                        played_from_browse = 1;
                        play_from_list(scroll + idx);
                    }
                } else if (screen == SC_PLAYLISTS && scroll + idx < playlist_n) {
                    static char paths[PAGE_MAX * 8][LIB_PATH_LEN];
                    int want = (int)(sizeof(paths) / sizeof(paths[0]));
                    int got = pl_read(playlists[scroll + idx].path, paths, want);
                    track_n = 0;
                    for (int k = 0; k < got; k++)
                        if (lib_track_by_path(paths[k], &tracks[track_n]) == 0)
                            track_n++;
                    /* A playlist is an order someone chose; leave it alone. */
                    snprintf(cur_album, sizeof(cur_album), "%s", playlists[scroll + idx].name);
                    cur_artist[0] = '\0';
                    screen = SC_TRACKS; reset_scroll();
                    ab_list = 0;
                    pod_list = 0;
                    mlog("[music] playlist %s: %d of %d found\n",
                         playlists[scroll + idx].name, track_n, got);
                } else if (screen == SC_RADIO && scroll + idx < station_n) {
                    play_station(scroll + idx);
                    if (audio_is_active()) screen = SC_PLAYING;
                    else if (!radio_msg[0])
                        snprintf(radio_msg, sizeof(radio_msg), "Could not reach that station");
                } else if (screen == SC_ALBUMS && row_at(scroll + idx)) {
                    lib_row_t *row = row_at(scroll + idx);
                    snprintf(cur_album, sizeof(cur_album), "%s", row->name);
                    /* Take the artist from the album row rather than from
                     * whatever facet led here: two artists can have an album
                     * of the same name, and the track query needs both.
                     * Unconditional, not "if row->owner[0]" -- BG30: an
                     * album with no album_artist tag at all (common; see the
                     * BG11 comment in library.c) used to leave cur_artist
                     * holding whatever a *previous* album or artist browse
                     * had set, which then stuck on the Now Playing screen
                     * indefinitely. row->owner empty here genuinely means
                     * "this album has no album_artist", and lib_tracks_for_
                     * album() already retries without the filter when an
                     * empty artist finds nothing, so writing "" through is
                     * correct, not just harmless. */
                    snprintf(cur_artist, sizeof(cur_artist), "%s", row->owner);
                    albums_scroll_saved = scroll;          /* BG37 follow-up */
                    albums_scroll_px_saved = scroll_px;
                    screen = SC_TRACKS; reset_scroll();
                    ab_list = 0;
                    pod_list = 0;
                    track_n = lib_tracks_for_album(cur_artist, cur_album,
                                                   tracks, (int)(sizeof(tracks)/sizeof(tracks[0])));
                    mlog("[music] %s -> %d tracks\n", cur_album, track_n);
                } else if (screen == SC_AUDIOBOOKS && scroll + idx < ab_book_n) {
                    ab_save_current_pos();       /* whatever was playing, before it's overwritten */
                    ab_book_t *b = &ab_books[scroll + idx];
                    ab_load_book(b);
                    audiobook_mode = 1;
                    audio_set_speed(ab_speed_permille);
                    ab_playing[0] = '\0';        /* force a real open */
                    queue_n = 0;                 /* force the mirror into queue[] */
                    screen = SC_PLAYING;
                    ab_resume_book();
                    mlog("[music] %s -> %d chapters in %d file(s)\n",
                         cur_album, ab_book.chap_n, ab_book.file_n);
                } else if (screen == SC_PODCASTS && scroll + idx < pod_feed_n) {
                    snprintf(cur_feed, sizeof(cur_feed), "%s", pod_feeds[scroll + idx].name);
                    pod_ep_n = pod_load_episodes(cur_feed, pod_eps, POD_MAX_ITEMS);
                    pod_rebuild_tracks();
                    snprintf(cur_album, sizeof(cur_album), "%s", cur_feed);
                    cur_artist[0] = '\0';
                    screen = SC_TRACKS; reset_scroll();
                    ab_list = 0;
                    pod_list = 1;
                    mlog("[music] %s -> %d episodes\n", cur_feed, pod_ep_n);
                } else if (screen == SC_EQ_BANDS && scroll + idx < eq_cur.band_n) {
                    /* The toggle's own strip (matching where it's drawn)
                     * flips the band without leaving the list; anywhere else
                     * on the row opens the full editor. */
                    if (x > FB_W - 100) {
                        eq_band_t *b = &eq_cur.band[scroll + idx];
                        b->on = !b->on;
                        eq_set_band(scroll + idx, b);
                        eq_save_current();
                    } else {
                        eq_editing_band = scroll + idx;
                        screen = SC_EQ_BAND; reset_scroll();
                    }
                }
            }
        }

        int keyed = 0;
        for (int k = 0; k < kfd_n; k++) {
            int r = handle_keys(kfd[k], kfd_src[k]);
            if (r < 0) {
                /* RBG1: this fd died (its device was torn down and
                 * recreated, e.g. a Bluetooth headset's AVRCP reconnect).
                 * Close it and drop it by swapping in the last entry, so
                 * the name frees up and the next scan_inputs() call below
                 * picks up a fresh, live handle instead of leaving this
                 * slot permanently dead for the rest of the app session. */
                close(kfd[k]);
                kfd_n--;
                kfd[k] = kfd[kfd_n];
                kfd_src[k] = kfd_src[kfd_n];
                snprintf(kfd_name[k], sizeof(kfd_name[0]), "%s", kfd_name[kfd_n]);
                k--;
                continue;
            }
            keyed |= r;
        }
        if (keyed) { dirty = 1; idle = 0; }

        /* A headset connecting brings its AVRCP device with it. */
        if (++rescan_tick >= 90) { rescan_tick = 0; scan_inputs(); }

        /* Regardless of pause state, so putting the device down mid-chapter
         * and never touching it again still gets saved -- the alternative
         * is trusting a clean exit, and a battery pull or a crash is exactly
         * what this exists to survive. Once every ~15s: cheap enough not to
         * matter, infrequent enough not to wear the card writing it. */
        if (++ab_pos_tick >= 450) { ab_pos_tick = 0; ab_save_current_pos(); pod_save_current_pos(); }

        /* Podcast downloads and whole-feed syncs both run as detached child
         * processes (see podcast.c) -- polled and reaped every tick, cheap
         * when idle since both are no-ops with nothing running. A completed
         * download re-reads the feed from disk rather than patching pod_eps[]
         * in place, the same one-shot recompute pattern SC_AUDIOBOOKS/
         * SC_PODCASTS already use after a rescan. */
        if (pod_download_poll() == 1 && pod_list) {
            pod_ep_n = pod_load_episodes(cur_feed, pod_eps, POD_MAX_ITEMS);
            pod_rebuild_tracks();
            dirty = 1;
        }
        pod_update_reap();
        if (pod_update_running()) {
            pod_sync_log_n = pod_update_tail(pod_sync_log, POD_SYNC_LOG_N);
            if (!pod_update_running() && (screen == SC_PODCASTS || screen == SC_POD_SYNC)) {
                /* Just finished: the feed list on screen may have new
                 * subfolders (a brand new feed) or new manifest-only
                 * episodes -- refresh it the same way opening Podcasts does. */
                pod_feed_n = pod_scan_feeds(pod_feeds, POD_MAX_FEEDS);
                pod_rebuild_rows();
                total = pod_feed_n;
            }
            dirty = 1;
        }

        /* Pulling the headphones out should not carry on broadcasting to the
         * room. Only for the wired route — unplugging the jack says nothing
         * about a Bluetooth or USB stream. */
        {
            int jack = st_headset();
            if (jack_was && !jack && !audio_is_paused() && audio_is_active() &&
                !strcmp(audio_output(), "3.5 mm")) {
                audio_toggle();
                mlog("[music] headphones removed, paused\n");
                dirty = 1;
            }
            jack_was = jack;
        }

        if (vol_ticks > 0) {
            if (!vol_dragging) vol_ticks--;
            if (vol_ticks == 0) dirty = 1;
        }
        if (vol_ticks > 0 && touch_down && !qs_open &&
            touch_y >= STATUS_H && touch_y < STATUS_H + VOL_H) {
            if (!vol_dragging) { vol_dragging = 1; vol_applied = audio_volume(); }
            int bw = FB_W - 48;
            int v = (live_x - 24) * 100 / (bw > 0 ? bw : 1);
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            if (v != vol_drag_pct) { vol_drag_pct = v; dirty = 1; }
            /* Applying this on every frame means a fork and exec of amixer,
             * twice over — set and read back — thirty times a second on
             * Bluetooth. The bar follows the finger; the mixer catches up a
             * few times a second, and exactly once more on release. */
            if (++vol_apply_tick >= 6) {
                vol_apply_tick = 0;
                if (vol_drag_pct != vol_applied) {
                    audio_volume_set(vol_drag_pct);
                    vol_applied = vol_drag_pct;
                }
            }
            idle = 0;
        } else if (vol_dragging && !touch_down) {
            vol_dragging = 0;
            if (vol_drag_pct != vol_applied) {
                audio_volume_set(vol_drag_pct);
                vol_applied = vol_drag_pct;
            }
            vol_ticks = VOL_TICKS;          /* restart the countdown on release */
        }

        /* Pull down from the status strip to open quick settings; the panel is
         * modal while it is open, so nothing below it needs to know. */
        if (!qs_open && touch_down && touch_y < QS_PULL_ZONE &&
            !(vol_ticks > 0 && touch_y >= STATUS_H) &&
            live_y - touch_y > QS_PULL) {
            qs_open = 1;
            qs_refresh();
            dirty = 1; idle = 0;
        }

        /* Dragging the brightness bar sets it live rather than on release: it
         * is the one setting where you want to see the result while choosing. */
        if (qs_open && qs_dragging && touch_down) {
            int bw = FB_W - 48;
            int v = (live_x - 24) * qs_bright_max / (bw > 0 ? bw : 1);
            if (v != qs_bright) {
                qs_bright = v < 1 ? 1 : (v > qs_bright_max ? qs_bright_max : v);
                st_brightness_set(qs_bright);
                saved_brightness = qs_bright;   /* so unlocking restores this */
                dirty = 1;
            }
            idle = 0;
        } else if (qs_dragging && !touch_down) {
            qs_dragging = 0;
        }

        /* Running down the A-Z strip. The test is on where the finger went
         * down, not where it is: a touchscreen only reports an axis that has
         * changed, so during a vertical drag the live X reading is whatever it
         * was last time and testing that missed the strip entirely. */
        {
            if (qs_open && touch_down && !qs_dragging &&
                touch_y > qs_bar_y() - 26 && touch_y < qs_bar_y() + 26)
                qs_dragging = 1;

            g_phase = "index";
            int was = index_active;
            index_active = touch_down && index_visible() &&
                           touch_x >= FB_W - INDEX_TOUCH_W &&
                           touch_y >= CONTENT_Y && touch_y < index_bottom();
            if (index_active) {
                /* Prepare only after this is known to be an A-Z gesture. */
                index_prepare();
                int prev_shown = index_shown;      /* only for the idx2 log below */

                /* Slot first, before anything reads index_shown/scroll/
                 * index_lock_end for a decision. A diagonal drag reports X
                 * and Y in the same tick, and deciding whether to lock
                 * using yesterday's letter while today's Y has already
                 * moved is exactly the bug this replaced: moving down while
                 * sliding left evaluated "should I lock" against whatever
                 * letter the thumb was over BEFORE the vertical move, so
                 * going left got silently ignored whenever it landed in the
                 * same tick as a vertical crossing. */
                int slot = index_slot(live_y);
                if (slot != index_shown) {
                    /* Live, not deferred. Waiting for the thumb to settle was
                     * meant to save two table scans per stop crossed, but the
                     * list lagging behind the droplet is the whole feel of the
                     * thing — and the scans it was protecting against are not
                     * the ones that ever cost anything. */
                    index_shown = slot;
                    g_phase = "index-jump";
                    index_jump(live_y);
                    dirty = 1;
                }

                /* Sliding left locks the strip inside the letter under the
                 * thumb: it stops being A-Z and becomes that letter's own
                 * second letters, which is then scrolled exactly like the
                 * top level. Sliding back out returns to A-Z. Everything
                 * here now reads index_shown/scroll/index_lock_end after
                 * the slot update above, so it always reflects this tick's
                 * position, not last tick's. */
                int deep = live_x < FB_W - INDEX_TOUCH_W - INDEX_SUB_DEAD;
                /* Only worth subdividing a letter that doesn't already fit
                 * on one screen. G has six albums and room for nine — sliding
                 * between Gi/Go/Gr subdivided it anyway, and switching bands
                 * that all draw from the same six albums looks exactly like
                 * unwanted scrolling even though it never reaches H.
                 * index_lock_end/scroll are already sitting from the
                 * top-level jump that necessarily happened before the thumb
                 * could slide left onto this letter, so no extra query. */
                int fits = index_lock_end >= 0 && index_lock_end - scroll <= vis_rows();
                if (deep && !index_sub_lock && index_shown > 0 && !fits) {
                    char L = index_letter(index_shown);
                    int n = 0;
                    for (int k = 0; k < index_pfx_n && n < INDEX_SUB_MAX; k++)
                        if (index_pfx[2 * k] == L) index_sub[n++] = index_pfx[2 * k + 1];
                    if (n > 1) {           /* one stop is not a subdivision */
                        index_sub_lock = L; index_sub_n = n;
                        /* Jump immediately rather than deferring to "next
                         * tick's slot != index_shown" — that deferral is
                         * exactly the kind of one-tick gap this fix removes. */
                        index_shown = -1;
                        index_jump(live_y);
                        dirty = 1;
                        mlog("[music] index: locked %c, %d stops\n", L, n);
                    }
                } else if (!deep && index_sub_lock) {
                    index_sub_lock = 0; index_sub_n = 0; index_lock_end = -1;
                    index_shown = -1;
                    index_jump(live_y);
                    dirty = 1;
                }

                if ((g_tick & 3) == 0)
                    mlog("[music] idx2 deep=%d fits=%d prev=%d shown=%d slot=%d lock=%c "
                         "lock_end=%d scroll=%d vis=%d live_x=%d live_y=%d\n",
                         deep, fits, prev_shown, index_shown, slot,
                         index_sub_lock ? index_sub_lock : '-',
                         index_lock_end, scroll, vis_rows(), live_x, live_y);
                idle = 0;
            } else if (was) {
                index_shown = -1;
                index_sub_lock = 0;
                index_sub_n = 0;
                index_lock_end = -1;
                dirty = 1;
            }
        }

        /* List scrolling: tracked live, one pixel at a time, rather than
         * jumping by whatever whole number of rows the release distance
         * happened to divide into — which is where "four items at a time"
         * came from, since an ordinary swipe covers roughly that many row
         * heights before the finger lifts. */
        {
            int scrollable = screen == SC_ARTISTS || screen == SC_ALBUMS ||
                             screen == SC_TRACKS || screen == SC_PLAYLISTS ||
                             screen == SC_RADIO || screen == SC_EQ_BANDS;
            int was = list_dragging;
            list_dragging = touch_down && scrollable && !index_active &&
                            !scrub_active && !qs_open &&
                            touch_y >= CONTENT_Y && !edge_active;
            if (list_dragging && !was) {
                /* A raw drag on the list itself is free browsing, not bound
                 * by wherever the index last landed — otherwise a stale
                 * boundary from an earlier jump could clip content that has
                 * nothing to do with it once scrolled somewhere else. */
                if (index_lock_end >= 0) dirty = 1;
                index_lock_end = -1;
                list_down_y = live_y;
                list_start_px = scroll * ROW_H + scroll_px;
                list_last_y = live_y;
                list_last_tick = g_tick;
                list_velocity = 0;
                inertia_active = 0;
            } else if (list_dragging) {
                if (scroll_to_px(list_start_px + (list_down_y - live_y))) dirty = 1;
                unsigned dt = g_tick - list_last_tick;
                if (dt > 0) {
                    /* Forward-positive: finger moving up (y decreasing)
                     * scrolls the list forward, same sign as the jump above. */
                    list_velocity = (float)(list_last_y - live_y) / (float)dt;
                    list_last_y = live_y;
                    list_last_tick = g_tick;
                }
                idle = 0;
            } else if (was) {
                /* Release: hand off to inertia if the finger was moving. */
                inertia_active = list_velocity > 0.6f || list_velocity < -0.6f;
                dirty = 1;
            }
        }
        if (inertia_active && !list_dragging) {
            if (scroll_to_px((int)(scroll * ROW_H + scroll_px + list_velocity))) dirty = 1;
            list_velocity *= 0.90f;     /* friction: dead within about a second */
            if (list_velocity < 0.6f && list_velocity > -0.6f) inertia_active = 0;
            idle = 0;
        }

        /* Show-notes scrolling: no inertia, no row snapping -- it's free text
         * in a fixed box, not a list, so the plain drag-follows-the-finger
         * half of the list-scrolling trick above is all this needs. Clamped
         * to [0, a generous upper bound] rather than measured exactly against
         * the wrapped text's real height, which pod_draw_notes() only knows
         * mid-draw -- an 8KB buffer can't wrap past roughly this many pixels
         * of lines, so overscroll into blank space is bounded, not unbounded. */
        {
            int notes_active = screen == SC_PLAYING && podcast_mode && pod_notes_showing;
            int was = pod_notes_dragging;
            pod_notes_dragging = touch_down && notes_active && touch_y < ART_PX;
            if (pod_notes_dragging && !was) {
                pod_notes_down_y = live_y;
                pod_notes_start_px = pod_notes_scroll_px;
            } else if (pod_notes_dragging) {
                int px = pod_notes_start_px + (pod_notes_down_y - live_y);
                if (px < 0) px = 0;
                if (px > 6000) px = 6000;
                if (px != pod_notes_scroll_px) { pod_notes_scroll_px = px; dirty = 1; }
                idle = 0;
            }
        }

        /* Dragging a long title sideways. Relative, not absolute: the string
         * follows the finger from wherever it was left, rather than jumping so
         * that some position maps to some offset -- which is what the scrub bar
         * and the sliders below want, but would be wrong here.
         *
         * touch_x is the position of the *press*, not the current finger, so
         * (live_x - touch_x) is travel since the drag began. The result is
         * committed on release, so the title stays where it was let go. */
        {
            int was = title_dragging;
            title_dragging = touch_down && screen == SC_PLAYING && !radio_mode &&
                             queue_n > 0 && title_span > 0 &&
                             touch_y > title_y() - 16 &&
                             touch_y < title_y() + TEXT_PX_TITLE + 16;
            if (title_dragging) {
                title_off_live = title_off + (live_x - touch_x);
                if (title_off_live > 0) title_off_live = 0;
                if (title_off_live < -title_span) title_off_live = -title_span;
                dirty = 1; idle = 0;
            } else if (was) {
                title_off = title_off_live;
                dirty = 1; idle = 0;
            }
        }

        /* Scrubbing: begins when the finger goes down on the bar and lasts
         * until it lifts. The seek is left to the release, which arrives as a
         * tap at the final position and is handled with the other taps. */
        {
            int was = scrub_active;
            scrub_active = touch_down && screen == SC_PLAYING && !radio_mode &&
                           queue_n > 0 &&
                           touch_y > bar_y() - 26 && touch_y < bar_y() + 26;
            if (scrub_active || was) { dirty = 1; idle = 0; }
        }

        /* Same shape as scrubbing, for the EQ sliders: which one (if any) the
         * finger is over this tick, so the draw code can show it tracking
         * live_x instead of the stored value. The actual write happens on
         * release, which arrives as a tap at the final position and is
         * handled with the other taps -- nothing new needed for that part. */
        {
            int was = eq_dragging;
            eq_dragging = 0;
            if (touch_down) {
                if (screen == SC_EQ) {
                    int pby = eq_preamp_y() + 26;
                    if (touch_y > pby - 20 && touch_y < pby + 20) eq_dragging = 1;
                } else if (screen == SC_EQ_BAND) {
                    int fy = eq_freq_y(), gy = eq_gain_y(), qy = eq_q_y();
                    if      (touch_y > fy - 20 && touch_y < fy + 20) eq_dragging = 2;
                    else if (touch_y > gy - 20 && touch_y < gy + 20) eq_dragging = 3;
                    else if (touch_y > qy - 20 && touch_y < qy + 20) eq_dragging = 4;
                }
            }
            if (eq_dragging || was) { dirty = 1; idle = 0; }
        }

        {
            int was = mseb_dragging;
            mseb_dragging = -1;
            if (touch_down && screen == SC_MSEB) {
                for (int i = 0; i < MSEB_BAND_N; i++) {
                    int sy = mseb_band_row_y(i) + 46;
                    if (touch_y > sy - 20 && touch_y < sy + 20) { mseb_dragging = i; break; }
                }
            }
            if (mseb_dragging >= 0 || was >= 0) { dirty = 1; idle = 0; }
        }

        /* Press-and-hold on a track: fires under the finger, once. */
        /* Not on a chapter list: "play next" and "add to queue" are about a
         * queue of tracks, and a book's chapters are not that — acting on
         * them would rewrite the chapter table out from under the player. */
        if (touch_down && !touch_moved && !edge_active && !hold_fired && !sheet_open &&
            screen == SC_TRACKS && !ab_list && touch_y >= CONTENT_Y) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long held = (now.tv_sec - touch_at.tv_sec) * 1000L +
                        (now.tv_nsec - touch_at.tv_nsec) / 1000000L;
            if (held >= HOLD_MS) {
                int idx = scroll + (touch_y - CONTENT_Y + scroll_px) / ROW_H;
                hold_fired = 1;
                if (idx < track_n) {
                    sheet_open = 1;
                    sheet_track = idx;
                    dirty = 1; idle = 0;
                }
            }
        }

        if (g) dirty = 1;
        {   /* the hint has to follow the finger, so redraw while it is up */
            static int hint_was;
            if (edge_active || hint_was) dirty = 1;
            hint_was = edge_active;
        }
        {
            int sec = audio_is_active() ? audio_pos_ms() / 1000 : -1;
            if (sec != last_sec) { last_sec = sec; dirty = 1; }
            int seq = art_seq();
            if (seq != art_seen) { art_seen = seq; dirty = 1; }
            /* The status bar changes on its own — battery drains, the jack is
             * pulled — so it gets a slow tick of its own rather than relying
             * on the clock, which only runs during playback. */
            if (++status_tick >= 60) {
                status_tick = 0;
                if (qs_open) qs_refresh();      /* the radios take a moment */
                /* Same reasoning as BG6's standby-undo below: whatever turns
                 * this LED on isn't this app, so a one-shot fix doesn't rule
                 * out it happening again later. Only while unlocked --
                 * led_pulse_step() below already reasserts far more often
                 * than this ~2s tick while locked, so this would otherwise
                 * fight the pulse with a periodic flat value. */
                if (!button_locked) { write_int_file(LED_BLUE, 0); write_int_file(LED_RED, 0); }
                dirty = 1;
            }
        }
        if (button_locked) led_pulse_step();

        if (!locked && idle_ticks > 0 && ++idle >= idle_ticks) set_locked(1);

        /* Low-power idle. Counted in loop ticks, and while locked the loop runs
         * at 10 Hz, which is the only rate that can ever reach the threshold --
         * the count resets the instant the device is unlocked or something
         * starts playing, so it can only ever accumulate at the locked rate. */
        /* "Nothing playing" has to mean *playing*, not "has a track loaded".
         * audio_is_active() stays true across a pause -- g_active is only
         * cleared when the worker exits -- so gating on it alone meant a book
         * left paused, which is precisely the case that flattened the battery
         * overnight, never reached the idle path at all. Paused is not
         * playing: the decoder is parked, the PCM has already been handed
         * back, and nothing is going to the amp. */
        int playing = audio_is_active() && !audio_is_paused();
        if (locked && !playing && sleep_minutes() > 0 && suspend_ok()) {
            if (++sleep_idle >= sleep_minutes() * 60 * 10) {
                deep_suspend();
                /* Resumed. Start the whole timeout again rather than suspending
                 * on the very next tick: the press that woke the device is a
                 * single press, which button_locked swallows, so an immediate
                 * re-suspend would make the thing effectively impossible to
                 * wake. A full idle period guarantees a window to get back in. */
                sleep_idle = 0;
            }
        } else {
            sleep_idle = 0;
        }

        /* Auto shutdown. Same gate and same tick-counted shape as the idle
         * timer above, but its own independent counter and its own setting --
         * a poweroff is not a suspend, does not care whether deep sleep is
         * enabled, and there is nothing to resume into afterwards, so unlike
         * sleep_idle there is no reset-and-repeat: this fires once. */
        if (locked && !playing && auto_off_minutes() > 0) {
            if (++auto_off_idle >= auto_off_minutes() * 60 * 10) {
                mlog("[music] auto shutdown after %d min idle\n", auto_off_minutes());
                /* Explicit rather than relying on the exit path below: there is
                 * no SIGTERM handler here, so whatever poweroff's shutdown
                 * sequence sends is not guaranteed to reach it, and the last
                 * periodic autosave could be up to 15s stale. This is exactly
                 * the position a resumed-from-suspend book needs to still be
                 * right, and it is nearly free to just save it fresh. */
                ab_save_current_pos();
                if (system("/sbin/poweroff") == -1) { }
                /* poweroff is not instant; keep the loop from re-firing this
                 * every tick while the shutdown sequence runs. */
                auto_off_idle = 0;
            }
        } else {
            auto_off_idle = 0;
        }

        /* BG6: undo the stock player's standby blanking.
         *
         * hiby_player runs its own standby timer, and while this app is open
         * that timer can never reset: EVIOCGRAB gives us *exclusive* input,
         * so the player sees no touches and no keys at all, however busy the
         * user actually is, and eventually blanks the panel out from under
         * us. No HiBy setting stops it, because from its point of view the
         * device really has been idle the whole time. It is not observable
         * as an event either — it is their code path on their timer, which
         * is why nothing appears in this log before the screen goes dark.
         *
         * So watch the backlight instead of trying to intercept the cause:
         * dark while we did not lock it means something else did. Restoring
         * needs the unblank as well as the brightness, since the player may
         * also have powered the framebuffer down (see set_locked). */
        if (++blank_tick >= 15) {                 /* ~0.5 s */
            blank_tick = 0;
            int b = read_int_file(BACKLIGHT);
            if (b > 0) {
                last_lit_bright = b;              /* track what the user chose */
            } else if (!locked) {
                mlog("[music] panel blanked by the player; restoring\n");
                if (g_fbfd >= 0) ioctl(g_fbfd, FBIOBLANK, FB_BLANK_UNBLANK);
                /* Through st_brightness_set(), same reason as set_locked():
                 * a cached last_lit_bright at the literal max would just
                 * write the same value that goes dark. */
                st_brightness_set(last_lit_bright > 0 ? last_lit_bright : DEFAULT_BRIGHTNESS);
            }
        }

        /* Nothing is drawn while the panel is dark; the frame is produced on
         * wake instead. */
        if (locked) dirty = 0;

        /* While a finger is down on the scrub bar or the volume slider, repaint
         * only that part, straight into the page already on screen. A full
         * frame is a screen clear, a cover blit and a page flip; these two are
         * a few tens of kilobytes. */
        /* Volume only. The same trick on the scrub bar left a grey track with
         * no fill: the strip function does not reproduce the state the full
         * draw sets up, so the duration resolved to zero and the filled part
         * was skipped. A bar that will not move is worse than one that moves
         * heavily, so scrubbing keeps the full redraw. */
        if (dirty && vol_dragging && !qs_open && frames > 0) {
            draw_volume(base + (size_t)(page ^ 1) * page_px);
            dirty = 0;
        }

        if (dirty) {
            /* Draw into the page that is not on screen, then flip to it.
             * Painting the visible page instead — briefly tried, to make the
             * panel and a screenshot agree — clears the whole frame before
             * repainting it, and the cover visibly flickered once a second as
             * the clock ticked. */
            draw_ui(base + (size_t)page * page_px);
            v.yoffset = (uint32_t)(page * FB_H);
            if (ioctl(fbfd, FBIOPAN_DISPLAY, &v) < 0 && frames < 3)
                mlog("[music] pan failed: %s\n", strerror(errno));
            /* Copy the finished frame to the other page as well. /dev/fb0
             * reports a single page to read(), so anything screenshotting the
             * device silently gets whichever page is not on screen; keeping
             * both identical means it gets the right picture either way.
             *
             * Not while a finger is dragging, though: it is three quarters of
             * a megabyte per frame on top of the repaint, and at thirty frames
             * a second that is what makes a drag feel like treacle. Nobody is
             * screenshotting mid-gesture. */
            /* Only where it paid for itself. Skipping the mirror during a
             * scrub also made screenshots read the stale spare page, which
             * cost an hour chasing a bar that was moving on the device and
             * not in the capture. */
            /* list_dragging/inertia_active belong here too: same 768 KB
             * mirror, paid every ~33ms tick throughout a scroll, and nobody
             * is screenshotting mid-drag any more here than during a volume
             * or quick-settings drag. */
            int dragging = vol_dragging || qs_dragging || list_dragging || inertia_active;
            if (!dragging)
                memcpy(base + (size_t)(page ^ 1) * page_px,
                       base + (size_t)page * page_px, page_px * 2);
            page ^= 1;
            frames++;
            dirty = 0;
        }
        /* 30 Hz is a frame rate, and with the panel dark there are no frames:
         * `if (locked) dirty = 0` above has already thrown the drawing away, so
         * what is left is polling. Slowing it to 10 Hz while locked cuts two
         * thirds of the wakeups out of the longest-running state this device
         * has -- sitting in a pocket -- and costs nothing visible, because the
         * only thing that can happen is a key press, input events queue in the
         * kernel until they are read, and the double-press window is measured
         * from the kernel's own timestamps rather than from when this loop
         * happened to look (see handle_keys). */
        usleep(locked ? 100000 : 33000);
    }

    ab_save_current_pos();     /* the periodic tick may be up to 15s stale */

    if (snapshot) {
        memcpy(base, snapshot, page_px * 2);
        memcpy(base + page_px, snapshot, page_px * 2);
        free(snapshot);
    }
    v.yoffset = 0;
    ioctl(fbfd, FBIOPAN_DISPLAY, &v);
    munmap(base, map_len);
    if (tfd >= 0) { ioctl(tfd, EVIOCGRAB, 0); close(tfd); }
    for (int i = 0; i < kfd_n; i++)
        if (kfd[i] >= 0) { ioctl(kfd[i], EVIOCGRAB, 0); close(kfd[i]); }
    kfd_n = 0;
    bt_poll_run = 0;
    if (bt_thread_valid) { pthread_join(bt_thread, NULL); bt_thread_valid = 0; }
    set_locked(0);                     /* never hand the panel back dark */
    close(fbfd);
    audio_stop();
    lib_close();
    mlog("[music] leaving app after %d frames\n", frames);
    return 0;
}

/* ---- tile name and icon -------------------------------------------------- */
/* The tile this app takes over is still labelled "Stream media" with a cloud
 * icon. The rootfs is read-only squashfs, so a bind mount is the only way to
 * change either short of reflashing, and it has to happen in this constructor
 * rather than from a boot script: the player reads its string table during
 * startup and a backgrounded script loses that race.
 *
 * The label is in sys_set.ini, not settings.ini or launcher.ini — which is
 * lucky, because the Podcasts app shadows settings.ini and two hooks binding
 * over the same file would leave one of the two names lost.
 */
#define RES_DIR    "/usr/data/music_res"
#define LABEL_INI  "/tmp/.music_sys_set.ini"
#define TILE_LABEL "Library"

/* The ini is UTF-16LE, so tags have to be matched widened. */
static size_t widen(const char *s, uint8_t *out) {
    size_t n = 0;
    for (; *s; s++) { out[n++] = (uint8_t)*s; out[n++] = 0; }
    return n;
}

static const uint8_t *memfind(const uint8_t *hay, size_t hn,
                              const uint8_t *needle, size_t nn) {
    if (nn > hn) return NULL;
    for (size_t i = 0; i + nn <= hn; i++)
        if (memcmp(hay + i, needle, nn) == 0) return hay + i;
    return NULL;
}

static const char *make_label_ini(void) {
    static const char SRC[] = "/usr/resource/str/english/sys_set.ini";
    int fd = open(SRC, O_RDONLY);
    if (fd < 0) return NULL;

    static uint8_t buf[16384];
    ssize_t len = read(fd, buf, sizeof(buf));
    close(fd);
    /* A full buffer means the file was truncated, and writing half a string
     * table would cost every label on the screen, not just this one. */
    if (len <= 0 || (size_t)len == sizeof(buf)) return NULL;

    uint8_t otag[48], ctag[48], label[64];
    size_t on = widen("<stream_media>", otag);
    size_t cn = widen("</stream_media>", ctag);
    size_t ln = widen(TILE_LABEL, label);

    const uint8_t *a = memfind(buf, (size_t)len, otag, on);
    if (!a) return NULL;
    const uint8_t *b = memfind(a, (size_t)len - (size_t)(a - buf), ctag, cn);
    if (!b) return NULL;

    size_t head = (size_t)(a - buf) + on;
    size_t tail = (size_t)len - (size_t)(b - buf);

    fd = open(LABEL_INI, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return NULL;
    int ok = write(fd, buf, head) == (ssize_t)head &&
             write(fd, label, ln) == (ssize_t)ln   &&
             write(fd, b, tail)   == (ssize_t)tail;
    close(fd);
    if (!ok) { unlink(LABEL_INI); return NULL; }
    return LABEL_INI;
}

static void shadow_resources(void) {
    static const char *pairs[][2] = {
        { RES_DIR "/stream_media.png",   "/usr/resource/litegui/theme1/launcher/stream_media.png" },
        { RES_DIR "/stream_media_s.png", "/usr/resource/litegui/theme1/launcher/stream_media_s.png" },
        { RES_DIR "/stream_media.png",   "/usr/resource/litegui/theme2/launcher/stream_media.png" },
        { RES_DIR "/stream_media_s.png", "/usr/resource/litegui/theme2/launcher/stream_media_s.png" },
    };
    for (unsigned i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        if (access(pairs[i][0], R_OK) != 0) continue;   /* no icon shipped: keep the stock one */
        if (mount(pairs[i][0], pairs[i][1], NULL, MS_BIND, NULL) != 0)
            mlog("[music] bind %s failed: %s\n", pairs[i][1], strerror(errno));
    }

    const char *ini = make_label_ini();
    if (!ini)
        mlog("[music] label rewrite failed; tile keeps its stock name\n");
    else if (mount(ini, "/usr/resource/str/english/sys_set.ini", NULL, MS_BIND, NULL) != 0)
        mlog("[music] bind sys_set.ini failed: %s\n", strerror(errno));
}

/* ---- install ------------------------------------------------------------- */
static void build_trampoline(uint32_t target, uint32_t *out) {
    out[0] = 0x3C190000u | ((target + 0x8000) >> 16);  /* lui   t9, hi     */
    out[1] = 0x27390000u | (target & 0xFFFF);          /* addiu t9, t9, lo */
    out[2] = 0x03200008u;                              /* jr    t9         */
    out[3] = 0x00000000u;                              /* nop              */
}

#ifndef __NR_cacheflush
#define __NR_cacheflush 4147
#endif
#define BCACHE_FLAG 3

static int is_hiby_player(void) {
    char buf[128];
    int fd = open("/proc/self/comm", O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    return strstr(buf, "hiby") != NULL || strstr(buf, "system_main") != NULL;
}

__attribute__((constructor))
static void music_init(void) {
    if (!is_hiby_player()) return;
    mlog("[music] init pid=%d entry=%p\n", (int)getpid(), &music_entry);

    volatile uint32_t *cave = (volatile uint32_t *)CAVE_ADDR;
    if (cave[0] != 0 || cave[1] != 0) { mlog("[music] cave occupied\n"); return; }

    if (mprotect((void *)CAVE_PAGE, PAGE_SPAN,
                 PROT_READ | PROT_WRITE | PROT_EXEC) < 0) {
        mlog("[music] cave mprotect failed\n");
        return;
    }
    uint32_t t[4];
    build_trampoline((uint32_t)&music_entry, t);
    cave[0] = t[0]; cave[1] = t[1]; cave[2] = t[2]; cave[3] = t[3];
    __asm__ __volatile__("sync" ::: "memory");
    syscall(__NR_cacheflush, (void *)CAVE_ADDR, 16, BCACHE_FLAG);
    mprotect((void *)CAVE_PAGE, PAGE_SPAN, PROT_READ | PROT_EXEC);

    volatile uint32_t *cb = (volatile uint32_t *)TILE_CB;
    if (*cb != TILE_CB_ORIG) {
        mlog("[music] unexpected tile callback 0x%08X, leaving it alone\n", *cb);
        return;
    }
    if (mprotect((void *)DATA_PAGE, PAGE_SPAN, PROT_READ | PROT_WRITE) < 0) {
        mlog("[music] data mprotect failed\n");
        return;
    }
    orig_cb = *cb;
    *cb = CAVE_ADDR;
    __asm__ __volatile__("sync" ::: "memory");
    /* If the player died while the screen was locked, the backlight was left
     * at 0 and nothing ever put it back: the device looks dead, and the only
     * way out is a reboot. Whatever happened last time, start lit. */
    if (read_int_file(BACKLIGHT) <= 0) {
        write_int_file(BACKLIGHT, DEFAULT_BRIGHTNESS);
        mlog("[music] backlight was off at startup; restored\n");
    }
    /* Stock firmware turns the blue one on before this constructor ever
     * runs, with no script found responsible for it -- off at startup
     * (button_locked can't be true this early). Both forced to manual mode
     * first: this session found the kernel's own "breathing" trigger a
     * no-op on this board, but nothing rules out something else along the
     * way leaving a trigger other than none selected, which would ignore
     * these raw brightness writes entirely. */
    write_text_file(LED_BLUE_TRIGGER, "none");
    write_text_file(LED_RED_TRIGGER, "none");
    write_int_file(LED_BLUE, 0);
    write_int_file(LED_RED, 0);

    shadow_resources();
    mlog("[music] Stream media tile armed -> 0x%08X\n", CAVE_ADDR);

    /* The firmware preloads exactly one library, and this app took the slot —
     * which silently removed the Podcasts app from the device. Rather than
     * reflash to add a second preload, load it from here: the two hooks patch
     * different tiles (About vs Stream media) into different code caves, and
     * both are built with hidden visibility so their internals cannot collide.
     * RTLD_LOCAL keeps it that way.
     *
     * Missing is not an error — plenty of installs will have only this app. */
    void *pod = dlopen(PODCAST_HOOK_PATH, RTLD_NOW | RTLD_LOCAL);
    mlog("[music] podcast hook: %s\n", pod ? "loaded" : dlerror());
}
