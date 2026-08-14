/* text.c — antialiased text via stb_truetype, using a font already on the device.
 *
 * The 5x7 bitmap font this replaces was uppercase-only ASCII, so German and
 * Swedish episode titles lost their accents ("träumen" -> "TRAUMEN").
 *
 * Font choice is constrained: /usr/resource/fonts/default.otf is CFF/PostScript
 * ("OTTO"), which stb_truetype cannot parse — it only handles `glyf` outlines.
 * msyh.ttf (Microsoft YaHei) is TrueType, covers Latin-1, and is already
 * present, so nothing has to be shipped.
 *
 * Glyphs are rasterised on first use and cached per (size, codepoint); the UI
 * only uses a handful of sizes and a few hundred characters.
 */

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "vendor/stb_truetype.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "text.h"

#define MAX_SIZES   4
/* Two ranges: Latin (0..0x24F) plus General Punctuation (0x2000..0x206F), which
 * carries the en/em dashes, curly quotes and ellipses that podcast titles are
 * full of. A flat array up to 0x2070 would be mostly empty, so the punctuation
 * block is folded in just above the Latin one. */
#define LATIN_CP    0x250
#define PUNCT_BASE  0x2000
#define PUNCT_LEN   0x70
#define MAX_CP      (LATIN_CP + PUNCT_LEN)

static int cp_slot(unsigned cp) {
    if (cp < LATIN_CP) return (int)cp;
    if (cp >= PUNCT_BASE && cp < PUNCT_BASE + PUNCT_LEN)
        return (int)(LATIN_CP + (cp - PUNCT_BASE));
    return -1;
}

typedef struct {
    uint8_t *bitmap;           /* 8-bit coverage, NULL until rasterised */
    int w, h, xoff, yoff, adv;
} glyph_t;

typedef struct {
    int      px;
    float    scale;
    int      ascent;
    glyph_t *glyphs;           /* MAX_CP entries */
} size_cache_t;

static stbtt_fontinfo g_font;
static const uint8_t *g_data;
static size_t g_len;
static int g_ready;
static size_cache_t g_sizes[MAX_SIZES];
static int g_size_count;

static const char *FONT_CANDIDATES[] = {
    "/usr/resource/fonts/msyh.ttf",
    "/usr/resource/fonts/Korean.ttf",
    "/usr/data/podcast_res/font.ttf",   /* optional override */
};

int text_init(void) {
    if (g_ready) return 0;
    for (unsigned i = 0; i < sizeof(FONT_CANDIDATES) / sizeof(FONT_CANDIDATES[0]); i++) {
        int fd = open(FONT_CANDIDATES[i], O_RDONLY);
        if (fd < 0) continue;
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size < 1024) { close(fd); continue; }
        const uint8_t *d = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (d == MAP_FAILED) continue;
        if (!stbtt_InitFont(&g_font, d, stbtt_GetFontOffsetForIndex(d, 0))) {
            munmap((void *)d, (size_t)st.st_size);
            continue;                  /* e.g. CFF outlines, which stb rejects */
        }
        g_data = d;
        g_len = (size_t)st.st_size;
        g_ready = 1;
        return 0;
    }
    return -1;
}

static size_cache_t *cache_for(int px) {
    for (int i = 0; i < g_size_count; i++)
        if (g_sizes[i].px == px) return &g_sizes[i];
    if (g_size_count >= MAX_SIZES) return &g_sizes[0];

    size_cache_t *c = &g_sizes[g_size_count];
    c->px = px;
    c->scale = stbtt_ScaleForPixelHeight(&g_font, (float)px);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&g_font, &asc, &desc, &gap);
    c->ascent = (int)(asc * c->scale);
    c->glyphs = calloc(MAX_CP, sizeof(glyph_t));
    if (!c->glyphs) return NULL;
    g_size_count++;
    return c;
}

static glyph_t *glyph_for(size_cache_t *c, unsigned cp) {
    if (!c || !c->glyphs) return NULL;
    int slot = cp_slot(cp);
    if (slot < 0) return NULL;
    glyph_t *g = &c->glyphs[slot];
    if (g->bitmap || g->adv) return g;          /* cached (space has no bitmap) */

    /* Non-breaking space has no glyph in this font, and asking for one draws
     * .notdef — a visible box mid-sentence. Treat it, and anything else the
     * font lacks, as a plain space. */
    unsigned draw_cp = (cp == 0x00A0) ? ' ' : cp;
    if (stbtt_FindGlyphIndex(&g_font, (int)draw_cp) == 0) draw_cp = ' ';

    int adv, lsb;
    stbtt_GetCodepointHMetrics(&g_font, (int)draw_cp, &adv, &lsb);
    g->adv = (int)(adv * c->scale + 0.5f);
    if (draw_cp != ' ')
        g->bitmap = stbtt_GetCodepointBitmap(&g_font, 0, c->scale, (int)draw_cp,
                                             &g->w, &g->h, &g->xoff, &g->yoff);
    return g;
}

/* Minimal UTF-8: enough for Latin-1 and Latin Extended-A. */
static const char *utf8_next(const char *s, unsigned *cp) {
    unsigned char b = (unsigned char)*s;
    if (b < 0x80)            { *cp = b;                    return s + 1; }
    if ((b & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *cp = ((b & 0x1Fu) << 6) | (s[1] & 0x3Fu);         return s + 2;
    }
    if ((b & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *cp = ((b & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
        return s + 3;
    }
    *cp = '?';
    return s + 1;
}

/* RGB565 blend; a is 0..255 coverage. */
static inline uint16_t blend(uint16_t dst, uint16_t src, unsigned a) {
    if (a >= 250) return src;
    if (a < 6) return dst;
    unsigned sr = (src >> 11) & 0x1F, sg = (src >> 5) & 0x3F, sb = src & 0x1F;
    unsigned dr = (dst >> 11) & 0x1F, dg = (dst >> 5) & 0x3F, db = dst & 0x1F;
    unsigned r = (sr * a + dr * (255 - a)) / 255;
    unsigned g = (sg * a + dg * (255 - a)) / 255;
    unsigned b = (sb * a + db * (255 - a)) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

int text_width(const char *s, int px) {
    if (!g_ready && text_init() != 0) return 0;
    size_cache_t *c = cache_for(px);
    if (!c) return 0;
    int w = 0;
    unsigned cp;
    while (*s) {
        s = utf8_next(s, &cp);
        glyph_t *g = glyph_for(c, cp);
        if (g) w += g->adv;
    }
    return w;
}

/* Draws until the next glyph would cross `limit`. Sets *stopped when it ran out
 * of room rather than out of string, which is what tells the caller a marker is
 * needed. `s` is only advanced once a glyph is committed, so a run that stops on
 * the very last character is still reported as truncated. */
static int draw_run(uint16_t *fb, int fb_w, int fb_h, int clip_top, int x, int y,
                    const char *s, uint16_t colour, size_cache_t *c,
                    int limit, int *stopped) {
    unsigned cp;
    if (stopped) *stopped = 0;
    while (*s) {
        const char *next = utf8_next(s, &cp);
        glyph_t *g = glyph_for(c, cp);
        if (!g) { s = next; continue; }
        if (x + g->adv > limit) { if (stopped) *stopped = 1; break; }
        s = next;

        if (g->bitmap) {
            int gx = x + g->xoff;
            int gy = y + c->ascent + g->yoff;
            for (int row = 0; row < g->h; row++) {
                int py = gy + row;
                if (py < clip_top || py >= fb_h) continue;
                const uint8_t *src = g->bitmap + (size_t)row * g->w;
                uint16_t *dst = fb + (size_t)py * fb_w;
                for (int col = 0; col < g->w; col++) {
                    int pxx = gx + col;
                    if (pxx < 0 || pxx >= limit) continue;
                    unsigned a = src[col];
                    if (a) dst[pxx] = blend(dst[pxx], colour, a);
                }
            }
        }
        x += g->adv;
    }
    return x;
}

/* Three ASCII dots rather than U+2026: the only usable face on this device is a
 * CJK font, where the real ellipsis is full-width and would sit oddly — the same
 * reason the feed parser folds curly quotes to their straight forms. */
#define ELLIPSIS "..."

int text_draw(uint16_t *fb, int fb_w, int fb_h, int clip_top, int x, int y,
              const char *s, uint16_t colour, int px, int right_edge) {
    if (!g_ready && text_init() != 0) return x;
    size_cache_t *c = cache_for(px);
    if (!c) return x;
    if (right_edge <= 0 || right_edge > fb_w) right_edge = fb_w;

    /* Reserve room for the marker only when the string actually overflows, so
     * text that fits is laid out exactly as before. */
    int ell_w = 0;
    if (text_width(s, px) > right_edge - x) {
        ell_w = text_width(ELLIPSIS, px);
        if (ell_w > right_edge - x) ell_w = 0;   /* not even room for dots */
    }

    int stopped = 0;
    x = draw_run(fb, fb_w, fb_h, clip_top, x, y, s, colour, c, right_edge - ell_w, &stopped);
    if (stopped && ell_w)
        x = draw_run(fb, fb_w, fb_h, clip_top, x, y, ELLIPSIS, colour, c, right_edge, NULL);
    return x;
}
