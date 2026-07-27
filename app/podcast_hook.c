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

#include "font5x7.h"
#include "audio.h"

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

#define PODCAST_DIR "/data/mnt/sd_0/Audiobooks"
#define RESUME_DIR  "/data/mnt/sd_0/.podsync"
#define RESUME_FILE RESUME_DIR "/resume.txt"
#define LOG_PATH    "/tmp/.podcast_hook.log"
#define SYNC_SCRIPT RESUME_DIR "/podsync_once.sh"
#define SYNC_LOG    "/tmp/.podsync_run.log"
#define UPDATE_BTN_H 52

#define MAX_ITEMS 64
#define NAME_LEN  64
#define PATH_LEN  384

#define HEADER_H 64
#define ROW_H    56
#define LIST_TOP HEADER_H
#define ROWS_VISIBLE ((FB_H - LIST_TOP) / ROW_H)

enum { SCREEN_FEEDS = 0, SCREEN_EPISODES, SCREEN_PLAYING, SCREEN_UPDATE };

static uint32_t orig_cb = 0;

static char feeds[MAX_ITEMS][NAME_LEN];
static int  feed_count;
static char episodes[MAX_ITEMS][NAME_LEN];
static int  episode_count;

static int screen = SCREEN_FEEDS;
static int feed_sel = -1;
static int ep_sel = -1;
static int scroll = 0;
static char cur_feed[NAME_LEN];
static char cur_path[PATH_LEN];
static int  update_running = 0;

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
static int resume_lookup(const char *path) {
    FILE *f = fopen(RESUME_FILE, "r");
    if (!f) return 0;
    char line[PATH_LEN + 32];
    int ms = 0;
    while (fgets(line, sizeof(line), f)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = '\0';
        char *p = tab + 1;
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        if (strcmp(p, path) == 0) { ms = atoi(line); break; }
    }
    fclose(f);
    return ms;
}

static void resume_store(const char *path, int ms) {
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
            char *tab = strchr(probe, '\t');
            if (tab) {
                char *p = tab + 1;
                char *nl = strchr(p, '\n');
                if (nl) *nl = '\0';
                if (strcmp(p, path) == 0) continue;   /* replaced below */
            }
            snprintf(keep[n++], PATH_LEN + 32, "%s", line);
        }
        fclose(f);
    }
    f = fopen(RESUME_FILE, "w");
    if (f) {
        if (ms > 3000) fprintf(f, "%d\t%s\n", ms, path);
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

static void draw_char(uint16_t *fb, int x, int y, char ch, uint16_t c, int scale) {
    const uint8_t *g = glyph_for(ch);
    for (int row = 0; row < GLYPH_H; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < GLYPH_W; col++)
            if (bits & (1 << (GLYPH_W - 1 - col)))
                fill_rect(fb, x + col * scale, y + row * scale, scale, scale, c);
    }
}

static void draw_text(uint16_t *fb, int x, int y, const char *s,
                      uint16_t c, int scale, int right_edge) {
    int adv = (GLYPH_W + 1) * scale;
    if (right_edge <= 0) right_edge = FB_W;
    for (const char *p = s; *p; p++) {
        if (x + GLYPH_W * scale > right_edge) break;
        draw_char(fb, x, y, *p, c, scale);
        x += adv;
    }
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
    draw_text(fb, 16, 22, title, COL_TEXT, 3, FB_W - 90);
    if (right) draw_text(fb, FB_W - 70, 24, right, COL_TEXT, 2, 0);
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
        draw_text(fb, 18, y + 18, items[idx], COL_TEXT, 2, FB_W - 16);
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
    draw_text(fb, 16, LIST_TOP + 20, cur_feed, COL_DIM, 1, FB_W - 16);
    draw_text(fb, 16, LIST_TOP + 44, t, COL_TEXT, 2, FB_W - 16);

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

    /* Controls: -30s | play/pause | +30s */
    int by = bar_y + 70, bh = 76, bw = (FB_W - 48) / 3;
    for (int i = 0; i < 3; i++)
        fill_rect(fb, 16 + i * (bw + 8), by, bw, bh, COL_BTN);
    draw_text(fb, 16 + 24, by + 28, "-30", COL_TEXT, 3, 0);
    draw_text(fb, 16 + (bw + 8) + 20, by + 28,
              audio_is_paused() ? "PLAY" : "PAUS", COL_TEXT, 2, 0);
    draw_text(fb, 16 + 2 * (bw + 8) + 24, by + 28, "+30", COL_TEXT, 3, 0);

    /* Speed control, full width under the transport row. */
    int sy = by + bh + 12;
    fill_rect(fb, 16, sy, FB_W - 32, 60, COL_BTN);
    char sp[24];
    float v = audio_speed();
    snprintf(sp, sizeof(sp), "SPEED %d.%02dX", (int)v, (int)((v - (int)v) * 100 + 0.5f));
    draw_text(fb, 40, sy + 22, sp, COL_TEXT, 2, FB_W - 40);

    const char *err = audio_error();
    if (err) draw_text(fb, 16, sy + 76, err, RGB(230, 120, 120), 2, FB_W - 16);
    else if (audio_is_loading())
        draw_text(fb, 16, sy + 76, "LOADING...", COL_ACCENT, 2, 0);
    else if (!audio_is_active())
        draw_text(fb, 16, sy + 76, "FINISHED", COL_DIM, 2, 0);
}

static void draw_update(uint16_t *fb) {
    draw_header(fb, "UPDATE", update_running ? NULL : "BACK");
    static char lines[14][NAME_LEN];
    int n = update_tail(lines, 14);
    for (int i = 0; i < n; i++)
        draw_text(fb, 16, LIST_TOP + 14 + i * 26, lines[i], COL_TEXT, 2, FB_W - 16);
    if (update_running)
        draw_text(fb, 16, FB_H - 40, "WORKING...", COL_ACCENT, 2, 0);
    else
        draw_text(fb, 16, FB_H - 40, "TAP BACK WHEN READY", COL_DIM, 2, 0);
}

static void draw_ui(uint16_t *fb) {
    fill_rect(fb, 0, 0, FB_W, FB_H, COL_BG);
    if (screen == SCREEN_FEEDS) {
        draw_header(fb, "PODCASTS", "EXIT");
        /* Update bar sits directly under the header, above the list. */
        fill_rect(fb, 0, LIST_TOP, FB_W, UPDATE_BTN_H, COL_BTN);
        draw_text(fb, 16, LIST_TOP + 16, "UPDATE FEEDS", COL_ACCENT, 2, FB_W - 16);
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

static void load_episodes(const char *feed) {
    episode_count = 0;
    char dir[PATH_LEN];
    snprintf(dir, sizeof(dir), "%s/%s", PODCAST_DIR, feed);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && episode_count < MAX_ITEMS) {
        if (e->d_name[0] == '.') continue;
        if (!is_audio(e->d_name)) continue;
        /* Show the title without its extension; the list is the only label. */
        snprintf(episodes[episode_count], NAME_LEN, "%s", e->d_name);
        char *dot = strrchr(episodes[episode_count], '.');
        if (dot) *dot = '\0';
        episode_count++;
    }
    closedir(d);
    sort_items(episodes, episode_count);
    plog("[podcast] %d episodes in %s\n", episode_count, feed);
}

/* ---- touch -------------------------------------------------------------- */
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
    if (cur_path[0] && audio_duration_ms() > 0) {
        int pos = audio_position_ms();
        /* Treat the last few seconds as finished so it restarts next time. */
        if (pos > audio_duration_ms() - 5000) pos = 0;
        resume_store(cur_path, pos);
    }
}

/* Returns 1 to keep running, 0 to leave the app. */
static int handle_tap(int x, int y) {
    if (screen == SCREEN_PLAYING) {
        int bar_y = LIST_TOP + 110, by = bar_y + 70, bh = 76;
        int bw = (FB_W - 48) / 3;
        if (y < HEADER_H) {
            save_position();
            audio_stop();
            screen = SCREEN_EPISODES;
            return 1;
        }
        if (y >= by - 8 && y <= by + bh + 8) {
            if (x < 16 + bw) audio_seek_relative(-30000);
            else if (x < 16 + 2 * (bw + 8)) audio_toggle_pause();
            else audio_seek_relative(30000);
            return 1;
        }
        int sy = by + bh + 12;
        if (y >= sy && y <= sy + 60) audio_cycle_speed();
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
            screen = SCREEN_EPISODES;
            ep_sel = -1;
            scroll = 0;
        }
    } else {
        if (idx >= 0 && idx < episode_count) {
            ep_sel = idx;
            /* episodes[] holds display names with the extension stripped, so
             * find the real file rather than reconstructing the name. */
            char dirp[PATH_LEN];
            snprintf(dirp, sizeof(dirp), "%s/%s", PODCAST_DIR, cur_feed);
            cur_path[0] = '\0';
            DIR *dd = opendir(dirp);
            if (dd) {
                struct dirent *de;
                while ((de = readdir(dd))) {
                    if (de->d_name[0] == '.' || !is_audio(de->d_name)) continue;
                    char stem[NAME_LEN];
                    snprintf(stem, sizeof(stem), "%s", de->d_name);
                    char *d2 = strrchr(stem, '.');
                    if (d2) *d2 = '\0';
                    if (strcmp(stem, episodes[idx]) == 0) {
                        snprintf(cur_path, sizeof(cur_path), "%s/%s", dirp, de->d_name);
                        break;
                    }
                }
                closedir(dd);
            }
            if (!cur_path[0]) return 1;
            int start = resume_lookup(cur_path);
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

    int page = 0, frames = 0, ticks = 0;
    for (;;) {
        int x, y;
        int g = tfd >= 0 ? read_gesture(tfd, &x, &y) : 0;
        if (g == 2) {
            /* Swipe up scrolls down. Playing screen has no list. */
            if (screen == SCREEN_FEEDS)         scroll_by(-y / ROW_H, feed_count);
            else if (screen == SCREEN_EPISODES) scroll_by(-y / ROW_H, episode_count);
        } else if (g == 1) {
            if (!handle_tap(x, y)) break;
        }

        /* Checkpoint the resume position periodically while playing. */
        if (screen == SCREEN_PLAYING && ++ticks >= 150) {
            ticks = 0;
            save_position();
        }

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

    if (snapshot) {
        memcpy(base, snapshot, page_px * 2);
        memcpy(base + page_px, snapshot, page_px * 2);
        free(snapshot);
    }
    v.yoffset = 0;
    ioctl(fbfd, FBIOPAN_DISPLAY, &v);
    munmap(base, map_len);
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

__attribute__((constructor))
static void podcast_init(void) {
    if (!is_hiby_player()) return;
    plog("[podcast] init pid=%d entry=%p\n", (int)getpid(), &podcast_entry);

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
