/* podcast_hook.c — a Podcasts app on the HiBy R1's About launcher tile.
 *
 * Preloaded into hiby_player by the supervisor (/usr/bin/hiby_player.sh), which
 * loads /usr/data/libpodcast_hook.so when present. Two hooks:
 *
 *   Tile route — the launcher tile table lives in .data as 96-byte records, but
 *     a tile's name string and its callback belong to different records: for a
 *     name at S, the callback is at S + 0x48. The About tile's callback is at
 *     0x00892570. A callback may not point into this shared object — the
 *     launcher then fails to render — so it is aimed at an unused, zeroed cave
 *     in hiby_player's own .rodata, into which we write a MIPS trampoline.
 *
 *   Drawing — hiby_player pans the framebuffer via ioctl(FBIOPAN_DISPLAY). We
 *     interpose ioctl and paint the target buffer just before the pan, so the
 *     player's display loop and the touch controller keep running. This is the
 *     approach the audiobook mod arrived at; suppressing the pan instead kills
 *     the touch hardware.
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
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/input.h>

#include "font5x7.h"

/* ---- hiby_player addresses (firmware 2.0.25) ---------------------------- */
#define ABOUT_CB_1      0x00892150u   /* second About record, not the live one */
#define ABOUT_CB_2      0x00892570u   /* the live About tile callback          */
#define ABOUT_CB_ORIG   0x0053BC20u
#define DATA_PAGE       0x00892000u
#define CAVE_ADDR       0x0075E400u
#define CAVE_PAGE       0x0075E000u
#define PAGE_SPAN       0x2000u
#define FB_MMAP_PTR     0x008B4C14u   /* .bss slot holding the fb mmap base */

/* ---- panel -------------------------------------------------------------- */
#define FB_W 480
#define FB_H 800
#define FB_PIXELS (FB_W * FB_H)

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define COL_BG      RGB(16, 16, 20)
#define COL_HEADER  RGB(120, 60, 200)
#define COL_TEXT    RGB(240, 240, 245)
#define COL_DIM     RGB(150, 150, 160)
#define COL_ROW_A   RGB(32, 32, 40)
#define COL_ROW_B   RGB(26, 26, 33)
#define COL_ACCENT  RGB(90, 200, 140)

#define PODCAST_DIR "/data/mnt/sd_0/Audiobooks"
#define LOG_PATH    "/tmp/.podcast_hook.log"

#define MAX_FEEDS 32
#define NAME_MAX_LEN 48

#define HEADER_H 64
#define ROW_H    56
#define LIST_TOP HEADER_H

static uint32_t orig_cb = 0;

static char feeds[MAX_FEEDS][NAME_MAX_LEN];
static int feed_count = 0;
static int selected = -1;

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

/* ---- drawing ------------------------------------------------------------ */
static void fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > FB_W) w = FB_W - x;
    if (y + h > FB_H) h = FB_H - y;
    for (int r = 0; r < h; r++) {
        uint16_t *p = fb + (y + r) * FB_W + x;
        for (int i = 0; i < w; i++) p[i] = c;
    }
}

static void draw_char(uint16_t *fb, int x, int y, char ch, uint16_t c, int scale) {
    const uint8_t *g = glyph_for(ch);
    for (int row = 0; row < GLYPH_H; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < GLYPH_W; col++) {
            if (bits & (1 << (GLYPH_W - 1 - col)))
                fill_rect(fb, x + col * scale, y + row * scale, scale, scale, c);
        }
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

static void draw_ui(uint16_t *fb) {
    fill_rect(fb, 0, 0, FB_W, FB_H, COL_BG);

    fill_rect(fb, 0, 0, FB_W, HEADER_H, COL_HEADER);
    draw_text(fb, 16, 22, "PODCASTS", COL_TEXT, 3, 0);
    /* Back affordance, top right. */
    draw_text(fb, FB_W - 70, 24, "EXIT", COL_TEXT, 2, 0);

    if (feed_count == 0) {
        draw_text(fb, 16, LIST_TOP + 30, "NO FEEDS FOUND", COL_DIM, 2, 0);
        draw_text(fb, 16, LIST_TOP + 60, &PODCAST_DIR[16], COL_DIM, 1, 0);
        return;
    }

    for (int i = 0; i < feed_count; i++) {
        int y = LIST_TOP + i * ROW_H;
        if (y + ROW_H > FB_H) break;
        fill_rect(fb, 0, y, FB_W, ROW_H - 2, (i & 1) ? COL_ROW_B : COL_ROW_A);
        if (i == selected)
            fill_rect(fb, 0, y, 6, ROW_H - 2, COL_ACCENT);
        draw_text(fb, 18, y + 18, feeds[i], COL_TEXT, 2, FB_W - 16);
    }
}

/* ---- feed list ---------------------------------------------------------- */
static void load_feeds(void) {
    feed_count = 0;
    DIR *d = opendir(PODCAST_DIR);
    if (!d) { plog("[podcast] opendir failed: %s\n", strerror(errno)); return; }
    struct dirent *e;
    while ((e = readdir(d)) && feed_count < MAX_FEEDS) {
        if (e->d_name[0] == '.') continue;
        /* Directories only; skip the app's own dotfiles and stray media. */
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", PODCAST_DIR, e->d_name);
        DIR *sub = opendir(path);
        if (!sub) continue;
        closedir(sub);
        strncpy(feeds[feed_count], e->d_name, NAME_MAX_LEN - 1);
        feeds[feed_count][NAME_MAX_LEN - 1] = '\0';
        feed_count++;
    }
    closedir(d);
    plog("[podcast] %d feeds\n", feed_count);
}

/* ---- touch -------------------------------------------------------------- */
/* Returns 1 on a completed tap, filling the tx and ty outputs. */
static int read_tap(int fd, int *tx, int *ty) {
    struct input_event ev;
    static int cx = -1, cy = -1, down = 0;
    while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_X) cx = ev.value;
        else if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_Y) cy = ev.value;
        else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            if (ev.value) down = 1;
            else if (down) {
                down = 0;
                if (cx >= 0 && cy >= 0) { *tx = cx; *ty = cy; return 1; }
            }
        }
    }
    return 0;
}

/* ---- tile entry --------------------------------------------------------- */
/* The tile callback runs on the player's UI thread, so blocking here stops the
 * player's own render loop — no more pans, frozen screen. The audiobook mod
 * solves this by driving the pan itself while the app is open, and so do we:
 * map the framebuffer, then draw and pan every frame from this loop. */
static int podcast_entry(void *arg0, void *arg1) {
    (void)arg0; (void)arg1;
    plog("[podcast] entering app\n");

    load_feeds();
    selected = -1;

    int fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd < 0) { plog("[podcast] no fb: %s\n", strerror(errno)); return 0; }

    struct fb_var_screeninfo v;
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &v) < 0) {
        plog("[podcast] vinfo failed\n"); close(fbfd); return 0;
    }
    plog("[podcast] fb %ux%u virt %ux%u bpp=%u\n",
         v.xres, v.yres, v.xres_virtual, v.yres_virtual, v.bits_per_pixel);

    size_t page_px = (size_t)FB_W * FB_H;
    size_t map_len = page_px * 2 /*bytes per px*/ * 2 /*pages*/;
    uint16_t *base = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (base == MAP_FAILED) {
        plog("[podcast] mmap failed: %s\n", strerror(errno));
        close(fbfd); return 0;
    }

    int tfd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    if (tfd < 0) {
        plog("[podcast] no touch: %s\n", strerror(errno));
    } else if (ioctl(tfd, EVIOCGRAB, 1) < 0) {
        /* Without an exclusive grab the launcher also sees our taps and
         * navigates behind us. */
        plog("[podcast] EVIOCGRAB failed: %s\n", strerror(errno));
    }

    int page = 0, frames = 0;
    for (;;) {
        int x, y;
        if (tfd >= 0 && read_tap(tfd, &x, &y)) {
            plog("[podcast] tap %d,%d\n", x, y);
            if (y < HEADER_H) break;
            int idx = (y - LIST_TOP) / ROW_H;
            if (idx >= 0 && idx < feed_count) selected = idx;
        }

        draw_ui(base + (size_t)page * page_px);
        v.yoffset = (uint32_t)(page * FB_H);
        if (ioctl(fbfd, FBIOPAN_DISPLAY, &v) < 0 && frames < 3)
            plog("[podcast] pan failed: %s\n", strerror(errno));
        page ^= 1;
        if (frames < 2) plog("[podcast] frame %d drawn\n", frames);
        frames++;
        usleep(33000);
    }

    /* Hand the display back: leave the player looking at page 0. */
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
    out[0] = 0x3C190000u | ((target + 0x8000) >> 16);  /* lui   t9, hi */
    out[1] = 0x27390000u | (target & 0xFFFF);          /* addiu t9, t9, lo */
    out[2] = 0x03200008u;                              /* jr    t9 */
    out[3] = 0x00000000u;                              /* nop */
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
    if (cave[0] != 0 || cave[1] != 0) {
        plog("[podcast] cave occupied\n");
        return;
    }
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
