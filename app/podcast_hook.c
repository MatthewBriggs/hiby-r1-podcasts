/* podcast_hook.c — a Podcasts app on the HiBy R1's About launcher tile.
 *
 * Preloaded into hiby_player by the supervisor (/usr/bin/hiby_player.sh), which
 * loads /usr/data/libpodcast_hook.so when present.
 *
 *   Tile route — launcher tiles are 96-byte records in .data, but a tile's name
 *     string and its callback belong to different records: for a name at S the
 *     callback is at S + 0x48. The About tile's live callback is 0x00892570. A
 *     callback may not point into this shared object (the launcher then fails to
 *     render), so it is aimed at an unused zeroed cave in hiby_player's own
 *     .rodata, into which we write a MIPS trampoline at load time.
 *
 *   Frames — the tile callback runs on the player's UI thread, so blocking there
 *     stops the player's render loop and nothing is ever panned. While the app is
 *     open it therefore owns the loop: mmap /dev/fb0, draw, FBIOPAN_DISPLAY, flip.
 *     The touch node is grabbed so taps do not also reach the launcher beneath.
 *
 * Build: see build.sh
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <sys/wait.h>
#include <sys/mount.h>

#include "text.h"
#include "audio.h"
#include "cover.h"

/* ---- hiby_player addresses (firmware 2.0.25) ---------------------------- */
#define ABOUT_CB_1      0x00892150u
#define ABOUT_CB_2      0x00892570u   /* the live About tile callback */
#define ABOUT_CB_ORIG   0x0053BC20u
#define DATA_PAGE       0x00892000u
#define CAVE_ADDR       0x0075E400u
#define CAVE_PAGE       0x0075E000u
#define PAGE_SPAN       0x2000u

/* ---- panel -------------------------------------------------------------- */
#define FB_W 480
#define FB_H 800

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define COL_BG      RGB(16, 16, 20)
#define COL_HEADER  RGB(120, 60, 200)
#define COL_TEXT    RGB(240, 240, 245)
#define COL_DIM     RGB(150, 150, 160)
#define COL_ROW_A   RGB(32, 32, 40)
#define COL_ROW_B   RGB(26, 26, 33)
#define COL_ACCENT  RGB(90, 200, 140)
#define COL_BAR_BG  RGB(50, 50, 60)
#define COL_BTN     RGB(45, 45, 58)

#define PODCAST_DIR "/data/mnt/sd_0/Podcasts"
#define RESUME_DIR  "/data/mnt/sd_0/.podsync"
#define RESUME_FILE RESUME_DIR "/resume.txt"
#define LOG_PATH    "/tmp/.podcast_hook.log"
#define RES_DIR     "/usr/data/podcast_res"
#define SYNC_SCRIPT RESUME_DIR "/podsync_once.sh"
#define SYNC_LOG    "/tmp/.podsync_run.log"
#define UPDATE_BTN_H 52
#define BTN_MARGIN   12
#define BTN_GAP       6

#define MAX_ITEMS 64
#define NAME_LEN  64
#define PATH_LEN  384

#define TEXT_PX_SMALL  22
#define TEXT_PX_BODY   36
#define TEXT_PX_TITLE  40
#define TEXT_PX_MED    30

#define HEADER_H 64
#define ROW_H    54
#define LIST_TOP HEADER_H
#define ROWS_VISIBLE ((FB_H - LIST_TOP) / ROW_H)

enum { SCREEN_FEEDS = 0, SCREEN_EPISODES, SCREEN_PLAYING, SCREEN_UPDATE };

static uint32_t orig_cb = 0;

static char feeds[MAX_ITEMS][NAME_LEN];
static int  feed_count;
static char episodes[MAX_ITEMS][NAME_LEN];
static char episode_paths[MAX_ITEMS][PATH_LEN];
static long episode_mtime[MAX_ITEMS];
static int  episode_resume[MAX_ITEMS];   /* ms, or POS_FINISHED, or 0 */
static int  episode_dur[MAX_ITEMS];
static int  episode_count;

static int screen = SCREEN_FEEDS;
static int feed_sel = -1;
static int ep_sel = -1;
static int scroll = 0;
static char cur_feed[NAME_LEN];
static char cur_path[PATH_LEN];
static int  update_running = 0;

static void plog(const char *fmt, ...);

#define NOTES_MAX_LINES 200
#define NOTES_LINE_H    36
static char notes_lines[NOTES_MAX_LINES][160];
static int  notes_count;
static int  notes_scroll;

/* Wrap the episode's notes sidecar to the screen width. Written by the fetcher
 * next to the audio, so it survives the card being read elsewhere. */
static void load_notes(const char *audio_path) {
    notes_count = 0;
    notes_scroll = 0;

    char p[PATH_LEN];
    snprintf(p, sizeof(p), "%s", audio_path);
    char *dot = strrchr(p, '.');
    if (!dot) return;
    snprintf(dot, sizeof(p) - (size_t)(dot - p), ".txt");

    FILE *f = fopen(p, "r");
    if (!f) return;
    static char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (!n) return;
    buf[n] = '\0';

    const int width = FB_W - 32;
    const int px = TEXT_PX_MED;
    char line[160];
    int len = 0;
    const char *w = buf;

    while (*w && notes_count < NOTES_MAX_LINES) {
        /* Take one whitespace-delimited word at a time. */
        while (*w == ' ' || *w == '\n' || *w == '\r' || *w == '\t') w++;
        if (!*w) break;
        const char *end = w;
        while (*end && *end != ' ' && *end != '\n' && *end != '\r' && *end != '\t') end++;
        int wl = (int)(end - w);
        if (wl > (int)sizeof(line) - 2) wl = (int)sizeof(line) - 2;

        char cand[160];
        if (len) snprintf(cand, sizeof(cand), "%.*s %.*s", len, line, wl, w);
        else     snprintf(cand, sizeof(cand), "%.*s", wl, w);

        if (text_width(cand, px) > width && len) {
            snprintf(notes_lines[notes_count++], sizeof(notes_lines[0]), "%.*s", len, line);
            snprintf(line, sizeof(line), "%.*s", wl, w);
            len = (int)strlen(line);
        } else {
            snprintf(line, sizeof(line), "%s", cand);
            len = (int)strlen(line);
        }
        w = end;
    }
    if (len && notes_count < NOTES_MAX_LINES)
        snprintf(notes_lines[notes_count++], sizeof(notes_lines[0]), "%s", line);
    plog("[podcast] notes: %d lines\n", notes_count);
}

/* ---- volume + screen lock ------------------------------------------------ */
#define BACKLIGHT "/sys/class/backlight/backlight_pwm0/brightness"
static int  vol_show_frames;     /* countdown: draw the volume overlay */
static int  vol_pct = -1;        /* last known, for the overlay */
static int  locked;              /* screen off, touch ignored, audio keeps going */
static int  saved_brightness = -1;
static int  g_fbfd = -1;         /* set once the frame loop owns /dev/fb0 */
#define DEFAULT_BRIGHTNESS 43    /* the panel's stock level */
static char bt_mixer_name[64];   /* bluealsa element, named after the device */

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
    fprintf(f, "%d\n", v);
    fclose(f);
}

/* The bluealsa mixer element is named after the connected device, so look it
 * up rather than hardcoding it. Empty means "no BT sink". */
static void find_bt_mixer(void) {
    bt_mixer_name[0] = '\0';
    FILE *p = popen("amixer -D bluealsa scontrols 2>/dev/null", "r");
    if (!p) return;
    char line[256];
    if (fgets(line, sizeof(line), p)) {
        char *q1 = strchr(line, '\'');
        char *q2 = q1 ? strchr(q1 + 1, '\'') : NULL;
        if (q1 && q2) {
            size_t n = (size_t)(q2 - q1 - 1);
            if (n >= sizeof(bt_mixer_name)) n = sizeof(bt_mixer_name) - 1;
            memcpy(bt_mixer_name, q1 + 1, n);
            bt_mixer_name[n] = '\0';
        }
    }
    pclose(p);
}

/* Read back the current level so the overlay shows something truthful. */
static int read_volume_pct(void) {
    char cmd[256];
    if (bt_mixer_name[0])
        snprintf(cmd, sizeof(cmd), "amixer -D bluealsa sget '%s' 2>/dev/null", bt_mixer_name);
    else
        snprintf(cmd, sizeof(cmd), "amixer sget Left 2>/dev/null");
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    char line[256];
    int pct = -1;
    while (fgets(line, sizeof(line), p)) {
        char *b = strchr(line, '[');
        if (b && strchr(b, '%')) { pct = atoi(b + 1); break; }
    }
    pclose(p);
    return pct;
}

#define VOL_FILE RESUME_DIR "/volume.txt"

static void adjust_volume(int delta_pct) {
    char cmd[320];
    if (!audio_using_bt()) {
        /* Wired: the DAC's volume registers are not wired up, so the samples
         * are scaled in software by the decoder instead. */
        int v = audio_volume() + delta_pct;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        audio_set_volume(v);
        vol_pct = v;
        vol_show_frames = 110;
        FILE *f = fopen(VOL_FILE, "w");
        if (f) { fprintf(f, "%d\n", v); fclose(f); }
        return;
    }
    if (bt_mixer_name[0]) {
        snprintf(cmd, sizeof(cmd), "amixer -D bluealsa sset '%s' %d%%%c >/dev/null 2>&1",
                 bt_mixer_name, delta_pct < 0 ? -delta_pct : delta_pct,
                 delta_pct < 0 ? '-' : '+');
    } else {
        snprintf(cmd, sizeof(cmd),
                 "amixer sset Left %d%%%c >/dev/null 2>&1; amixer sset Right %d%%%c >/dev/null 2>&1",
                 delta_pct < 0 ? -delta_pct : delta_pct, delta_pct < 0 ? '-' : '+',
                 delta_pct < 0 ? -delta_pct : delta_pct, delta_pct < 0 ? '-' : '+');
    }
    if (system(cmd) == -1) return;
    vol_pct = read_volume_pct();
    vol_show_frames = 110;             /* ~3.5s at 30fps */
}

/* Locking only drops the backlight, which keeps waking cheap and lets audio run
 * on undisturbed. Waking has to do more: the player can power the framebuffer
 * down underneath us on its own display timeout, and restoring brightness to a
 * blanked panel leaves a black screen that no amount of pressing will bring
 * back. So unblank unconditionally — it costs nothing when the panel was never
 * blanked, and it is the difference between a dark screen and a dead one. */
static void set_locked(int on) {
    if (on == locked) return;
    locked = on;
    if (on) {
        saved_brightness = read_int_file(BACKLIGHT);
        write_int_file(BACKLIGHT, 0);
    } else {
        if (g_fbfd >= 0) ioctl(g_fbfd, FBIOBLANK, FB_BLANK_UNBLANK);
        /* A brightness of 0 or an unreadable node would otherwise restore to
         * darkness, which is indistinguishable from the failure above. */
        write_int_file(BACKLIGHT,
                       saved_brightness > 0 ? saved_brightness : DEFAULT_BRIGHTNESS);
    }
    plog("[podcast] %s\n", on ? "locked" : "unlocked");
}

#define COVER_PX 150
static uint16_t *cur_cover;      /* RGB565 square for the open feed, or NULL */
static char      cover_feed[NAME_LEN];

/* Decode once per feed; cover_load also caches the result on the card. */
static void cover_for_feed(const char *feed) {
    if (cur_cover && strcmp(cover_feed, feed) == 0) return;
    free(cur_cover);
    cur_cover = NULL;
    snprintf(cover_feed, sizeof(cover_feed), "%s", feed);
    char p[PATH_LEN];
    snprintf(p, sizeof(p), "%s/%s/cover.jpg", PODCAST_DIR, feed);
    cur_cover = cover_load(p, COVER_PX);
    plog("[podcast] cover %s: %s\n", feed, cur_cover ? "ok" : "none");
}

static void blit_cover(uint16_t *fb, int x, int y, int px) {
    if (!cur_cover) return;
    for (int r = 0; r < px; r++) {
        int sy = r * COVER_PX / px;
        for (int c = 0; c < px; c++) {
            int sx = c * COVER_PX / px;
            int dx = x + c, dy = y + r;
            if (dx < 0 || dy < 0 || dx >= FB_W || dy >= FB_H) continue;
            fb[(size_t)dy * FB_W + dx] = cur_cover[(size_t)sy * COVER_PX + sx];
        }
    }
}

/* ---- logging ------------------------------------------------------------ */
static void plog(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    int fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { write(fd, buf, n); close(fd); }
}

static int is_hiby_player(void) {
    int fd = open("/proc/self/comm", O_RDONLY);
    if (fd < 0) return 0;
    char b[64];
    int n = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0) return 0;
    b[n] = '\0';
    return strstr(b, "hiby_player") || strstr(b, "system_main_thr");
}

/* ---- resume positions ---------------------------------------------------- */
/* One line per episode: "<ms>\t<path>". Kept on the SD card so it survives the
 * internal data partition filling up, which this device is prone to. */
/* Lines are "<ms>\t<duration_ms>\t<path>". Older two-field lines still parse,
 * they just have no duration and so show no percentage. */
static int resume_lookup2(const char *path, int *dur_out) {
    if (dur_out) *dur_out = 0;
    FILE *f = fopen(RESUME_FILE, "r");
    if (!f) return 0;
    char line[PATH_LEN + 48];
    int ms = 0;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1 = '\0';
        char *rest = t1 + 1;
        char *t2 = strchr(rest, '\t');
        int dur = 0;
        char *pathp;
        if (t2) { *t2 = '\0'; dur = atoi(rest); pathp = t2 + 1; }
        else    { pathp = rest; }
        if (strcmp(pathp, path) == 0) {
            ms = atoi(line);
            if (dur_out) *dur_out = dur;
            break;
        }
    }
    fclose(f);
    return ms;
}

static int resume_lookup(const char *path) { return resume_lookup2(path, NULL); }

#define POS_FINISHED (-1)
static void resume_store(const char *path, int ms, int dur) {
    mkdir(RESUME_DIR, 0755);
    char (*keep)[PATH_LEN + 32] = malloc(sizeof(*keep) * 128);
    if (!keep) return;
    int n = 0;
    FILE *f = fopen(RESUME_FILE, "r");
    if (f) {
        char line[PATH_LEN + 32];
        while (n < 127 && fgets(line, sizeof(line), f)) {
            char probe[PATH_LEN + 32];
            snprintf(probe, sizeof(probe), "%s", line);
            char *nl = strchr(probe, '\n');
            if (nl) *nl = '\0';
            char *t1 = strchr(probe, '\t');
            if (t1) {
                char *rest = t1 + 1;
                char *t2 = strchr(rest, '\t');
                char *pathp = t2 ? t2 + 1 : rest;
                if (strcmp(pathp, path) == 0) continue;   /* replaced below */
            }
            snprintf(keep[n++], PATH_LEN + 32, "%s", line);
        }
        fclose(f);
    }
    f = fopen(RESUME_FILE, "w");
    if (f) {
        if (ms > 3000 || ms == POS_FINISHED) fprintf(f, "%d\t%d\t%s\n", ms, dur, path);
        for (int i = 0; i < n; i++) fputs(keep[i], f);
        fclose(f);
    }
    free(keep);
}

/* ---- update ------------------------------------------------------------- */
/* The fetcher is a shell script rather than C: it already existed, and it needs
 * the static curl on the card because busybox wget's TLS is too old for any
 * modern podcast host. Run it detached and follow its log. */
static void update_start(void) {
    if (update_running) return;
    unlink(SYNC_LOG);
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", SYNC_SCRIPT, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        update_running = 1;
        screen = SCREEN_UPDATE;
        plog("[podcast] update started pid=%d\n", (int)pid);
    }
}

/* Last few lines of the updater's log, newest last. */
static int update_tail(char out[][NAME_LEN], int max_lines) {
    FILE *f = fopen(SYNC_LOG, "r");
    if (!f) return 0;
    char line[256];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (!line[0]) continue;
        if (strcmp(line, "__DONE__") == 0) { update_running = 0; continue; }
        if (n < max_lines) {
            snprintf(out[n++], NAME_LEN, "%s", line);
        } else {
            for (int i = 1; i < max_lines; i++)
                memcpy(out[i - 1], out[i], NAME_LEN);
            snprintf(out[max_lines - 1], NAME_LEN, "%s", line);
        }
    }
    fclose(f);
    return n;
}

static int is_audio(const char *n);

/* ---- drawing ------------------------------------------------------------ */
static void fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > FB_W) w = FB_W - x;
    if (y + h > FB_H) h = FB_H - y;
    if (w <= 0 || h <= 0) return;
    for (int r = 0; r < h; r++) {
        uint16_t *p = fb + (y + r) * FB_W + x;
        for (int i = 0; i < w; i++) p[i] = c;
    }
}

/* The old bitmap font took a 1..3 "scale"; keep call sites unchanged by mapping
 * those onto pixel heights that match the previous layout closely. */
static int scale_px(int scale) {
    switch (scale) {
        case 1:  return TEXT_PX_SMALL;
        case 3:  return TEXT_PX_TITLE;
        case 4:  return TEXT_PX_MED;
        default: return TEXT_PX_BODY;
    }
}

/* Vertically centre body text in a list row. */
#define ROW_TEXT_Y ((ROW_H - 2 - TEXT_PX_BODY) / 2)

static void draw_text(uint16_t *fb, int x, int y, const char *s,
                      uint16_t c, int scale, int right_edge) {
    text_draw(fb, FB_W, FB_H, x, y, s, c, scale_px(scale), right_edge);
}

static void draw_play_icon(uint16_t *fb, int cx, int cy, int h, uint16_t col) {
    int w = h;                       /* apex sits w to the right of the edge */
    int x0 = cx - w / 2;
    for (int dy = -h; dy <= h; dy++) {
        int d = dy < 0 ? -dy : dy;
        int len = w * (h - d) / h;
        if (len > 0) fill_rect(fb, x0, cy + dy, len, 1, col);
    }
}

static void draw_pause_icon(uint16_t *fb, int cx, int cy, int h, uint16_t col) {
    int bw = h / 2, gap = h / 2;
    fill_rect(fb, cx - gap / 2 - bw, cy - h, bw, h * 2, col);
    fill_rect(fb, cx + gap / 2,      cy - h, bw, h * 2, col);
}

/* Horizontally centred on cx. The old bitmap font had a fixed advance so
 * labels were positioned by hand; real metrics make this exact. */
static void draw_text_centre(uint16_t *fb, int cx, int y, const char *s,
                             uint16_t c, int scale) {
    int px = scale_px(scale);
    text_draw(fb, FB_W, FB_H, cx - text_width(s, px) / 2, y, s, c, px, FB_W);
}

static void fmt_time(char *out, size_t n, int ms) {
    if (ms < 0) ms = 0;
    int s = ms / 1000, h = s / 3600, m = (s % 3600) / 60;
    s %= 60;
    if (h > 0) snprintf(out, n, "%d:%02d:%02d", h, m, s);
    else       snprintf(out, n, "%d:%02d", m, s);
}

static void draw_header(uint16_t *fb, const char *title, const char *right) {
    fill_rect(fb, 0, 0, FB_W, HEADER_H, COL_HEADER);
    draw_text(fb, 16, (HEADER_H - TEXT_PX_TITLE) / 2, title, COL_TEXT, 3, FB_W - 90);
    if (right) draw_text(fb, FB_W - 78, (HEADER_H - TEXT_PX_BODY) / 2, right, COL_TEXT, 2, 0);
}

/* Progress marker for an episode row: solid = finished, partial bar = started.
 * Reads the cache filled by load_episodes; doing the lookup here would mean a
 * directory scan and a file read per row per frame. */
static void draw_progress_marker(uint16_t *fb, int x, int y, int idx) {
    if (idx < 0 || idx >= episode_count) return;
    int ms = episode_resume[idx];
    if (ms == 0) return;                       /* untouched */
    if (ms < 0) {
        draw_text(fb, x - 26, y - 4, "DONE", COL_ACCENT, 2, FB_W);
        return;
    }
    int dur = episode_dur[idx];
    if (dur > 0) {
        int pct = (int)((int64_t)ms * 100 / dur);
        if (pct > 99) pct = 99;
        char t[8];
        snprintf(t, sizeof(t), "%d%%", pct);
        draw_text(fb, x - (pct >= 10 ? 26 : 14), y - 4, t, COL_ACCENT, 2, FB_W);
    } else {
        fill_rect(fb, x, y, 14, 14, COL_BAR_BG);
        fill_rect(fb, x, y + 7, 14, 7, COL_ACCENT);
    }
}

static void draw_list(uint16_t *fb, char items[][NAME_LEN], int count, int sel) {
    int top = LIST_TOP + (screen == SCREEN_FEEDS ? UPDATE_BTN_H : 0);
    if (count == 0) {
        draw_text(fb, 16, top + 30, "NOTHING HERE", COL_DIM, 2, 0);
        return;
    }
    int visible = (FB_H - top) / ROW_H;
    for (int i = 0; i < visible; i++) {
        int idx = scroll + i;
        if (idx >= count) break;
        int y = top + i * ROW_H;
        fill_rect(fb, 0, y, FB_W, ROW_H - 2, (idx & 1) ? COL_ROW_B : COL_ROW_A);
        if (idx == sel) fill_rect(fb, 0, y, 6, ROW_H - 2, COL_ACCENT);
        int right = FB_W - 16;
        if (screen == SCREEN_EPISODES) {
            draw_progress_marker(fb, FB_W - 20, y + ROW_TEXT_Y + 4, idx);
            right = FB_W - 70;
        }
        draw_text(fb, 18, y + ROW_TEXT_Y, items[idx], COL_TEXT, 2, right);
    }
    if (count > visible) {
        /* Scrollbar: proportional thumb down the right edge. */
        int track = FB_H - top;
        int th = track * visible / count;
        int ty = top + track * scroll / count;
        fill_rect(fb, FB_W - 5, LIST_TOP, 4, track, COL_ROW_B);
        fill_rect(fb, FB_W - 5, ty, 4, th < 20 ? 20 : th, COL_DIM);
    }
}

static void draw_playing(uint16_t *fb) {
    draw_header(fb, "PLAYING", "BACK");

    char t[NAME_LEN];
    snprintf(t, sizeof(t), "%s", ep_sel >= 0 ? episodes[ep_sel] : "");
    int art = cur_cover ? 84 : 0;
    if (art) blit_cover(fb, 16, LIST_TOP + 12, art);
    int tx = 16 + (art ? art + 12 : 0);
    draw_text(fb, tx, LIST_TOP + 12, cur_feed, COL_TEXT, 4, FB_W - 16);
    draw_text(fb, tx, LIST_TOP + 50, t, COL_DIM, 2, FB_W - 16);

    int pos = audio_position_ms(), dur = audio_duration_ms();
    int bar_y = LIST_TOP + 110, bar_h = 12;
    fill_rect(fb, 16, bar_y, FB_W - 32, bar_h, COL_BAR_BG);
    if (dur > 0) {
        int w = (int)((int64_t)(FB_W - 32) * pos / dur);
        fill_rect(fb, 16, bar_y, w, bar_h, COL_ACCENT);
    }
    char a[16], b[16];
    fmt_time(a, sizeof(a), pos);
    fmt_time(b, sizeof(b), dur);
    draw_text(fb, 16, bar_y + 24, a, COL_DIM, 2, 0);
    draw_text(fb, FB_W - 100, bar_y + 24, b, COL_DIM, 2, 0);

    /* Controls: -30 | -10 | play/pause | +10 | +30. The 10s nudges are what
     * you reach for after missing a sentence; 30s is for skipping an ad. */
    int by = bar_y + 70, bh = 76;
    int bw = (FB_W - 2 * BTN_MARGIN - 4 * BTN_GAP) / 5;
    const char *labels[5] = { "-30", "-10", NULL, "+10", "+30" };
    int label_y = by + (bh - TEXT_PX_BODY) / 2;
    for (int i = 0; i < 5; i++) {
        int bx = BTN_MARGIN + i * (bw + BTN_GAP);
        fill_rect(fb, bx, by, bw, bh, COL_BTN);
        if (labels[i]) {
            draw_text_centre(fb, bx + bw / 2, label_y, labels[i], COL_TEXT, 2);
        } else if (audio_is_paused()) {
            draw_play_icon(fb, bx + bw / 2, by + bh / 2, 16, COL_ACCENT);
        } else {
            draw_pause_icon(fb, bx + bw / 2, by + bh / 2, 16, COL_ACCENT);
        }
    }

    /* Speed control, full width under the transport row. */
    int sy = by + bh + 12;
    int sh = 66;
    fill_rect(fb, 16, sy, FB_W - 32, sh, COL_BTN);
    char sp[24];
    float v = audio_speed();
    snprintf(sp, sizeof(sp), "SPEED %d.%02dx", (int)v, (int)((v - (int)v) * 100 + 0.5f));
    draw_text_centre(fb, FB_W / 2, sy + (sh - TEXT_PX_BODY) / 2, sp, COL_TEXT, 2);

    const char *err = audio_error();
    int notes_top = sy + sh + 10;
    if (err) {
        draw_text(fb, 16, notes_top, err, RGB(230, 120, 120), 2, FB_W - 16);
    } else if (audio_is_loading()) {
        draw_text(fb, 16, notes_top, "LOADING...", COL_ACCENT, 2, 0);
    } else if (notes_count > 0) {
        int rows = (FB_H - notes_top) / NOTES_LINE_H;
        for (int i = 0; i < rows; i++) {
            int idx = notes_scroll + i;
            if (idx >= notes_count) break;
            draw_text(fb, 16, notes_top + i * NOTES_LINE_H, notes_lines[idx],
                      COL_TEXT, 4, FB_W - 16);
        }
        if (notes_count > rows) {
            int track = FB_H - notes_top;
            int th = track * rows / notes_count;
            int ty = notes_top + track * notes_scroll / notes_count;
            fill_rect(fb, FB_W - 5, notes_top, 4, track, COL_ROW_B);
            fill_rect(fb, FB_W - 5, ty, 4, th < 20 ? 20 : th, COL_ROW_A);
        }
    } else if (!audio_is_active()) {
        draw_text(fb, 16, notes_top, "FINISHED", COL_DIM, 2, 0);
    }
}

static void draw_update(uint16_t *fb) {
    draw_header(fb, "UPDATE", update_running ? NULL : "BACK");
    static char lines[14][NAME_LEN];
    int n = update_tail(lines, 14);
    for (int i = 0; i < n; i++)
        draw_text(fb, 16, LIST_TOP + 12 + i * 30, lines[i], COL_TEXT, 2, FB_W - 16);
    if (update_running)
        draw_text(fb, 16, FB_H - 40, "WORKING...", COL_ACCENT, 2, 0);
    else
        draw_text(fb, 16, FB_H - 40, "TAP BACK WHEN READY", COL_DIM, 2, 0);
}

static void draw_volume_overlay(uint16_t *fb) {
    if (vol_show_frames <= 0) return;
    vol_show_frames--;
    int h = 70, y = FB_H - h - 20;
    fill_rect(fb, 20, y, FB_W - 40, h, COL_BTN);
    char t[32];
    snprintf(t, sizeof(t), "VOL %d%%", vol_pct < 0 ? 0 : vol_pct);
    draw_text(fb, 36, y + 12, t, COL_TEXT, 2, FB_W - 40);
    int bw = FB_W - 72, bx = 36, by = y + 44;
    fill_rect(fb, bx, by, bw, 12, COL_BAR_BG);
    if (vol_pct > 0) fill_rect(fb, bx, by, bw * vol_pct / 100, 12, COL_ACCENT);
}

static void draw_ui(uint16_t *fb) {
    fill_rect(fb, 0, 0, FB_W, FB_H, COL_BG);
    if (screen == SCREEN_FEEDS) {
        draw_header(fb, "PODCASTS", "EXIT");
        /* Update bar sits directly under the header, above the list. */
        fill_rect(fb, 0, LIST_TOP, FB_W, UPDATE_BTN_H, COL_BTN);
        draw_text(fb, 16, LIST_TOP + (UPDATE_BTN_H - TEXT_PX_BODY) / 2, "UPDATE FEEDS", COL_ACCENT, 2, FB_W - 16);
        draw_list(fb, feeds, feed_count, feed_sel);
        if (feed_count == 0)
            draw_text(fb, 16, LIST_TOP + UPDATE_BTN_H + 60, &PODCAST_DIR[16], COL_DIM, 1, 0);
    } else if (screen == SCREEN_UPDATE) {
        draw_update(fb);
    } else if (screen == SCREEN_EPISODES) {
        draw_header(fb, cur_feed, "BACK");
        draw_list(fb, episodes, episode_count, ep_sel);
    } else {
        draw_playing(fb);
    }
    draw_volume_overlay(fb);
}

/* ---- listing ------------------------------------------------------------ */
static int is_audio(const char *n) {
    const char *d = strrchr(n, '.');
    if (!d) return 0;
    return !strcasecmp(d, ".mp3") || !strcasecmp(d, ".m4a") ||
           !strcasecmp(d, ".m4b") || !strcasecmp(d, ".aac") ||
           !strcasecmp(d, ".ogg") || !strcasecmp(d, ".opus") ||
           !strcasecmp(d, ".wav") || !strcasecmp(d, ".flac");
}

static void sort_items(char items[][NAME_LEN], int n) {
    for (int i = 1; i < n; i++) {
        char tmp[NAME_LEN];
        snprintf(tmp, sizeof(tmp), "%s", items[i]);
        int j = i - 1;
        while (j >= 0 && strcasecmp(items[j], tmp) > 0) {
            snprintf(items[j + 1], NAME_LEN, "%s", items[j]);
            j--;
        }
        snprintf(items[j + 1], NAME_LEN, "%s", tmp);
    }
}

static void load_feeds(void) {
    feed_count = 0;
    DIR *d = opendir(PODCAST_DIR);
    if (!d) { plog("[podcast] opendir: %s\n", strerror(errno)); return; }
    struct dirent *e;
    while ((e = readdir(d)) && feed_count < MAX_ITEMS) {
        if (e->d_name[0] == '.') continue;
        char path[PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", PODCAST_DIR, e->d_name);
        DIR *sub = opendir(path);
        if (!sub) continue;
        closedir(sub);
        snprintf(feeds[feed_count++], NAME_LEN, "%s", e->d_name);
    }
    closedir(d);
    sort_items(feeds, feed_count);
    plog("[podcast] %d feeds\n", feed_count);
}

/* Newest first, by file mtime. Sorting by name only looks right when episode
 * titles happen to start with a number; mtime reflects download order, which is
 * what a podcast listener expects. Names and paths move together. */
static void sort_episodes(void) {
    for (int i = 1; i < episode_count; i++) {
        char tn[NAME_LEN], tp[PATH_LEN];
        long tm = episode_mtime[i];
        snprintf(tn, sizeof(tn), "%s", episodes[i]);
        snprintf(tp, sizeof(tp), "%s", episode_paths[i]);
        int j = i - 1;
        while (j >= 0 && episode_mtime[j] < tm) {
            snprintf(episodes[j + 1], NAME_LEN, "%s", episodes[j]);
            snprintf(episode_paths[j + 1], PATH_LEN, "%s", episode_paths[j]);
            episode_mtime[j + 1] = episode_mtime[j];
            j--;
        }
        snprintf(episodes[j + 1], NAME_LEN, "%s", tn);
        snprintf(episode_paths[j + 1], PATH_LEN, "%s", tp);
        episode_mtime[j + 1] = tm;
    }
}

static void load_episodes(const char *feed) {
    episode_count = 0;
    char dir[PATH_LEN];
    snprintf(dir, sizeof(dir), "%s/%s", PODCAST_DIR, feed);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    /* Nothing is ever deleted automatically, so a long-running feed will exceed
     * MAX_ITEMS. Stopping at the first MAX_ITEMS the directory happens to list
     * would then hide new episodes behind old ones — readdir order on FAT32 is
     * roughly creation order, not date order, and the sort below only reorders
     * whatever survived. So once full, displace the oldest entry instead. */
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (!is_audio(e->d_name)) continue;

        char path[PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        long mt = (stat(path, &st) == 0) ? (long)st.st_mtime : 0;

        int slot;
        if (episode_count < MAX_ITEMS) {
            slot = episode_count++;
        } else {
            slot = 0;
            for (int i = 1; i < MAX_ITEMS; i++)
                if (episode_mtime[i] < episode_mtime[slot]) slot = i;
            if (mt <= episode_mtime[slot]) continue;   /* older than all we hold */
        }

        snprintf(episode_paths[slot], PATH_LEN, "%s", path);
        episode_mtime[slot] = mt;
        /* Show the title without its extension; the list is the only label. */
        snprintf(episodes[slot], NAME_LEN, "%s", e->d_name);
        char *dot = strrchr(episodes[slot], '.');
        if (dot) *dot = '\0';
    }
    closedir(d);
    sort_episodes();
    for (int i = 0; i < episode_count; i++)
        episode_resume[i] = resume_lookup2(episode_paths[i], &episode_dur[i]);
    plog("[podcast] %d episodes in %s\n", episode_count, feed);
}

/* ---- touch -------------------------------------------------------------- */
/* Volume and power live on the gpio/adc key nodes, not the touch node. */
static int read_key(int *fds, int n, int *code) {
    struct input_event ev;
    for (int i = 0; i < n; i++) {
        if (fds[i] < 0) continue;
        while (read(fds[i], &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_KEY && ev.value == 1) { *code = ev.code; return 1; }
        }
    }
    return 0;
}

/* Returns 1 for a tap, 2 for a vertical swipe (dy in *ty). */
static int read_gesture(int fd, int *tx, int *ty) {
    struct input_event ev;
    static int cx = -1, cy = -1, down = 0, sx = 0, sy = 0;
    while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_X) cx = ev.value;
        else if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_Y) cy = ev.value;
        else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            if (ev.value) { down = 1; sx = cx; sy = cy; }
            else if (down) {
                down = 0;
                if (cx < 0 || cy < 0) continue;
                int dy = cy - sy;
                if (dy > 40 || dy < -40) { *ty = dy; return 2; }
                *tx = cx; *ty = cy; return 1;
            }
        }
    }
    return 0;
}

static void scroll_by(int rows, int count) {
    scroll += rows;
    int max = count - ROWS_VISIBLE;
    if (max < 0) max = 0;
    if (scroll > max) scroll = max;
    if (scroll < 0) scroll = 0;
}


static void save_position(void) {
    if (!cur_path[0] || audio_duration_ms() <= 0) return;
    int pos = audio_position_ms();
    /* A sentinel rather than 0, so "finished" and "never started" stay
     * distinguishable in the episode list. */
    if (pos > audio_duration_ms() - 5000) pos = POS_FINISHED;

    /* Checkpointing runs on a timer, not on movement, so a paused episode left
     * on this screen would rewrite the whole file to the card every few seconds
     * with a value that never changes. */
    static int  last_ms = -1;
    static char last_path[PATH_LEN];
    if (pos == last_ms && strcmp(last_path, cur_path) == 0) return;

    resume_store(cur_path, pos, audio_duration_ms());
    last_ms = pos;
    snprintf(last_path, sizeof(last_path), "%s", cur_path);
}

/* Returns 1 to keep running, 0 to leave the app. */
static int handle_tap(int x, int y) {
    if (screen == SCREEN_PLAYING) {
        int bar_y = LIST_TOP + 110, by = bar_y + 70, bh = 76;
        if (y < HEADER_H) {
            save_position();
            audio_stop();
            if (ep_sel >= 0 && ep_sel < episode_count)
                episode_resume[ep_sel] =
                    resume_lookup2(episode_paths[ep_sel], &episode_dur[ep_sel]);
            screen = SCREEN_EPISODES;
            return 1;
        }
        if (y >= by - 8 && y <= by + bh + 8) {
            int bw5 = (FB_W - 2 * BTN_MARGIN - 4 * BTN_GAP) / 5;
            int idx = (x - BTN_MARGIN) / (bw5 + BTN_GAP);
            if (idx < 0) idx = 0;
            if (idx > 4) idx = 4;
            switch (idx) {
                case 0: audio_seek_relative(-30000); break;
                case 1: audio_seek_relative(-10000); break;
                case 2: audio_toggle_pause();        break;
                case 3: audio_seek_relative(10000);  break;
                default: audio_seek_relative(30000); break;
            }
            return 1;
        }
        int sy = by + bh + 12;
        if (y >= sy && y <= sy + 66) audio_cycle_speed();
        return 1;
    }

    if (screen == SCREEN_UPDATE) {
        if (y < HEADER_H && !update_running) {
            load_feeds();                 /* pick up anything just downloaded */
            screen = SCREEN_FEEDS;
            scroll = 0;
        }
        return 1;
    }

    if (y < HEADER_H) {
        if (screen == SCREEN_EPISODES) { screen = SCREEN_FEEDS; scroll = 0; return 1; }
        return 0;                                  /* EXIT from the feed list */
    }

    if (screen == SCREEN_FEEDS && y < LIST_TOP + UPDATE_BTN_H) {
        update_start();
        return 1;
    }

    int top = LIST_TOP + (screen == SCREEN_FEEDS ? UPDATE_BTN_H : 0);
    int idx = scroll + (y - top) / ROW_H;
    if (screen == SCREEN_FEEDS) {
        if (idx >= 0 && idx < feed_count) {
            feed_sel = idx;
            snprintf(cur_feed, sizeof(cur_feed), "%s", feeds[idx]);
            load_episodes(cur_feed);
            cover_for_feed(cur_feed);
            screen = SCREEN_EPISODES;
            ep_sel = -1;
            scroll = 0;
        }
    } else {
        if (idx >= 0 && idx < episode_count) {
            ep_sel = idx;
            snprintf(cur_path, sizeof(cur_path), "%s", episode_paths[idx]);
            load_notes(cur_path);
            int start = resume_lookup(cur_path);
            if (start < 0) start = 0;   /* finished: play again from the top */
            plog("[podcast] play %s from %dms\n", cur_path, start);
            audio_play(cur_path, start);
            screen = SCREEN_PLAYING;
        }
    }
    return 1;
}

/* ---- tile entry --------------------------------------------------------- */
static int podcast_entry(void *arg0, void *arg1) {
    (void)arg0; (void)arg1;
    plog("[podcast] entering app\n");

    screen = SCREEN_FEEDS;
    feed_sel = ep_sel = -1;
    scroll = 0;
    cur_path[0] = '\0';
    load_feeds();

    int fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd < 0) { plog("[podcast] no fb: %s\n", strerror(errno)); return 0; }
    g_fbfd = fbfd;                      /* so unlocking can unblank the panel */

    struct fb_var_screeninfo v;
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &v) < 0) {
        plog("[podcast] vinfo failed\n"); close(fbfd); return 0;
    }

    size_t page_px = (size_t)FB_W * FB_H;
    size_t map_len = page_px * 2 * 2;
    uint16_t *base = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (base == MAP_FAILED) {
        plog("[podcast] mmap failed: %s\n", strerror(errno));
        close(fbfd); return 0;
    }

    /* Keep a copy of whatever the launcher had on screen. On exit we put it
     * back, otherwise our last frame stays up until the player happens to
     * repaint, which can be seconds. */
    uint16_t *snapshot = malloc(page_px * 2);
    if (snapshot) memcpy(snapshot, base + (size_t)v.yoffset * FB_W, page_px * 2);

    int tfd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    if (tfd >= 0 && ioctl(tfd, EVIOCGRAB, 1) < 0)
        plog("[podcast] EVIOCGRAB failed: %s\n", strerror(errno));

    /* Grabbed for the duration: the player reads these nodes too and would
     * otherwise consume the presses (and adjust its own volume behind us). The
     * grab is released on exit, including the error paths below. */
    int kfds[2];
    kfds[0] = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    kfds[1] = open("/dev/input/event2", O_RDONLY | O_NONBLOCK);
    for (int i = 0; i < 2; i++) {
        if (kfds[i] < 0) { plog("[podcast] key fd %d open failed\n", i); continue; }
        if (ioctl(kfds[i], EVIOCGRAB, 1) < 0)
            plog("[podcast] key grab %d failed: %s\n", i, strerror(errno));
    }
    find_bt_mixer();
    {   /* Restore the saved software level for wired playback. */
        FILE *f = fopen(VOL_FILE, "r");
        if (f) { int v; if (fscanf(f, "%d", &v) == 1) audio_set_volume(v); fclose(f); }
    }
    vol_pct = audio_using_bt() ? read_volume_pct() : audio_volume();

    int page = 0, frames = 0, ticks = 0;
    for (;;) {
        int kc;
        while (read_key(kfds, 2, &kc)) {
            if (locked) {
                /* Any key wakes the screen; nothing else acts while locked. */
                set_locked(0);
                continue;
            }
            plog("[podcast] key %d\n", kc);
            if (kc == KEY_VOLUMEUP)        adjust_volume(+4);
            else if (kc == KEY_VOLUMEDOWN) adjust_volume(-4);
            else if (kc == KEY_POWER)      set_locked(1);
        }

        int x, y;
        int g = (tfd >= 0 && !locked) ? read_gesture(tfd, &x, &y) : 0;
        if (g == 2) {
            /* Swipe up scrolls down. */
            if (screen == SCREEN_FEEDS)         scroll_by(-y / ROW_H, feed_count);
            else if (screen == SCREEN_EPISODES) scroll_by(-y / ROW_H, episode_count);
            else if (screen == SCREEN_PLAYING && notes_count) {
                notes_scroll += -y / NOTES_LINE_H;
                if (notes_scroll > notes_count - 1) notes_scroll = notes_count - 1;
                if (notes_scroll < 0) notes_scroll = 0;
            }
        } else if (g == 1) {
            if (!handle_tap(x, y)) break;
        }

        /* Checkpoint the resume position periodically while playing. */
        if (screen == SCREEN_PLAYING && ++ticks >= 150) {
            ticks = 0;
            save_position();
        }

        if (locked) { usleep(120000); continue; }

        draw_ui(base + (size_t)page * page_px);
        v.yoffset = (uint32_t)(page * FB_H);
        if (ioctl(fbfd, FBIOPAN_DISPLAY, &v) < 0 && frames < 3)
            plog("[podcast] pan failed: %s\n", strerror(errno));
        page ^= 1;
        frames++;
        usleep(33000);
    }

    save_position();
    audio_stop();
    free(cur_cover);
    cur_cover = NULL;
    cover_feed[0] = '\0';

    if (snapshot) {
        memcpy(base, snapshot, page_px * 2);
        memcpy(base + page_px, snapshot, page_px * 2);
        free(snapshot);
    }
    v.yoffset = 0;
    ioctl(fbfd, FBIOPAN_DISPLAY, &v);
    munmap(base, map_len);
    set_locked(0);                       /* never leave the panel dark */
    for (int i = 0; i < 2; i++)
        if (kfds[i] >= 0) { ioctl(kfds[i], EVIOCGRAB, 0); close(kfds[i]); }
    if (tfd >= 0) { ioctl(tfd, EVIOCGRAB, 0); close(tfd); }
    close(fbfd);
    plog("[podcast] leaving app after %d frames\n", frames);
    return 0;
}

/* ---- install ------------------------------------------------------------ */
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

/* ---- tile label --------------------------------------------------------- */
/* The tile is renamed by editing one string in the firmware's own settings.ini
 * rather than by shipping a modified copy: that file is HiBy's, and deriving the
 * replacement at runtime also means a firmware update can never leave a stale
 * string table bind-mounted over the new one. */
#define LABEL_INI   "/tmp/.podcast_settings.ini"
#define TILE_LABEL  "Podcasts"

/* settings.ini is UTF-16LE, so tags have to be matched widened. */
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
    static const char SRC[] = "/usr/resource/str/english/settings.ini";
    int fd = open(SRC, O_RDONLY);
    if (fd < 0) return NULL;

    static uint8_t buf[8192];
    ssize_t len = read(fd, buf, sizeof(buf));
    close(fd);
    /* A full buffer means the file was truncated, and writing a half a string
     * table would cost every label on the screen, not just this one. */
    if (len <= 0 || (size_t)len == sizeof(buf)) return NULL;

    uint8_t otag[32], ctag[32], label[64];
    size_t on = widen("<about>", otag);
    size_t cn = widen("</about>", ctag);
    size_t ln = widen(TILE_LABEL, label);

    const uint8_t *a = memfind(buf, (size_t)len, otag, on);
    if (!a) return NULL;
    const uint8_t *b = memfind(a, (size_t)len - (size_t)(a - buf), ctag, cn);
    if (!b) return NULL;

    size_t head = (size_t)(a - buf) + on;          /* up to and including <about> */
    size_t tail = (size_t)len - (size_t)(b - buf); /* from </about> to EOF        */

    fd = open(LABEL_INI, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return NULL;
    int ok = write(fd, buf, head)  == (ssize_t)head &&
             write(fd, label, ln)  == (ssize_t)ln   &&
             write(fd, b, tail)    == (ssize_t)tail;
    close(fd);
    if (!ok) { unlink(LABEL_INI); return NULL; }
    return LABEL_INI;
}

/* Shadow the stock tile icon and label. The rootfs is read-only squashfs, so a
 * bind mount is the only way short of reflashing. Doing it here rather than from
 * a boot script matters: this constructor runs before main(), and the player
 * reads its string table during startup — a backgrounded boot script lost that
 * race for the label (though not for the icon, which is loaded later). */
static void shadow_resources(void) {
    static const char *pairs[][2] = {
        { RES_DIR "/about.png",   "/usr/resource/litegui/theme1/launcher/about.png" },
        { RES_DIR "/about_s.png", "/usr/resource/litegui/theme1/launcher/about_s.png" },
        { RES_DIR "/about.png",   "/usr/resource/litegui/theme2/launcher/about.png" },
        { RES_DIR "/about_s.png", "/usr/resource/litegui/theme2/launcher/about_s.png" },
    };
    for (unsigned i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        if (access(pairs[i][0], R_OK) != 0) continue;
        if (mount(pairs[i][0], pairs[i][1], NULL, MS_BIND, NULL) != 0)
            plog("[podcast] bind %s failed: %s\n", pairs[i][1], strerror(errno));
    }

    /* Launcher tile labels live in settings.ini (music / net_set / sys_set /
     * about), not launcher.ini — that one drives a different menu, which is why
     * editing its <abo_dev> had no effect. */
    const char *ini = make_label_ini();
    if (!ini)
        plog("[podcast] label rewrite failed; tile keeps its stock name\n");
    else if (mount(ini, "/usr/resource/str/english/settings.ini",
                   NULL, MS_BIND, NULL) != 0)
        plog("[podcast] bind settings.ini failed: %s\n", strerror(errno));
}

__attribute__((constructor))
static void podcast_init(void) {
    if (!is_hiby_player()) return;
    plog("[podcast] init pid=%d entry=%p\n", (int)getpid(), &podcast_entry);
    shadow_resources();

    volatile uint32_t *cave = (volatile uint32_t *)CAVE_ADDR;
    if (cave[0] != 0 || cave[1] != 0) { plog("[podcast] cave occupied\n"); return; }

    if (mprotect((void *)CAVE_PAGE, PAGE_SPAN,
                 PROT_READ | PROT_WRITE | PROT_EXEC) < 0) {
        plog("[podcast] cave mprotect failed\n");
        return;
    }
    uint32_t t[4];
    build_trampoline((uint32_t)&podcast_entry, t);
    cave[0] = t[0]; cave[1] = t[1]; cave[2] = t[2]; cave[3] = t[3];
    __asm__ __volatile__("sync" ::: "memory");
    syscall(__NR_cacheflush, (void *)CAVE_ADDR, 16, BCACHE_FLAG);
    mprotect((void *)CAVE_PAGE, PAGE_SPAN, PROT_READ | PROT_EXEC);

    volatile uint32_t *c1 = (volatile uint32_t *)ABOUT_CB_1;
    volatile uint32_t *c2 = (volatile uint32_t *)ABOUT_CB_2;
    if (*c2 != ABOUT_CB_ORIG) {
        plog("[podcast] unexpected About callback 0x%08X\n", *c2);
        return;
    }
    if (mprotect((void *)DATA_PAGE, PAGE_SPAN, PROT_READ | PROT_WRITE) < 0) {
        plog("[podcast] data mprotect failed\n");
        return;
    }
    orig_cb = *c2;
    *c2 = CAVE_ADDR;
    if (*c1 == ABOUT_CB_ORIG) *c1 = CAVE_ADDR;
    __asm__ __volatile__("sync" ::: "memory");
    plog("[podcast] About tile armed -> 0x%08X\n", CAVE_ADDR);
}
