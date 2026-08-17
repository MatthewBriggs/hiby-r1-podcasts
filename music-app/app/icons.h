/* icons.h — small bitmap icons (Bluetooth, USB, Wi-Fi, volume, battery),
 * rasterized offline from Font Awesome Free SVGs by tools/icons/gen_icons.py.
 *
 * Nothing on this device can decode an SVG or a PNG -- only JPEG (cover.c)
 * and the audio formats. Every other icon in this app is drawn procedurally
 * (draw_bt_icon-style line/circle code). This is the exception, for shapes
 * that read badly as a handful of line segments at 16-30px: a Bluetooth
 * rune's crossed strokes are forgiving of that treatment, but the battery
 * fill bars and the USB trident are not.
 *
 * Each icon is stored as 8-bit alpha coverage, exactly like a text.c glyph
 * bitmap, and draw_icon() blends it the same way draw_run() blends a glyph:
 * source icons are solid single-color shapes, so coverage alone is the
 * shape and no RGB needs storing. That also means every icon recolors for
 * free with the theme/state color passed at draw time, the same as text. */
#ifndef ICONS_H
#define ICONS_H
#include <stdint.h>

typedef struct { int w, h; const uint8_t *a; } icon_t;

#include "icons_data.h"

/* Same fixed-point RGB565 blend as text.c's blend()/div255(), duplicated
 * rather than shared: both are ten-line statics, one per file that needs
 * them, matching how this codebase already keeps small helpers local. */
static inline unsigned icon_div255(unsigned t) { return (t + 1 + (t >> 8)) >> 8; }

static inline uint16_t icon_blend(uint16_t dst, uint16_t src, unsigned a) {
    if (a >= 250) return src;
    if (a < 6) return dst;
    unsigned sr = (src >> 11) & 0x1F, sg = (src >> 5) & 0x3F, sb = src & 0x1F;
    unsigned dr = (dst >> 11) & 0x1F, dg = (dst >> 5) & 0x3F, db = dst & 0x1F;
    unsigned na = 255 - a;
    unsigned r = icon_div255(sr * a + dr * na);
    unsigned g = icon_div255(sg * a + dg * na);
    unsigned b = icon_div255(sb * a + db * na);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* fb_w/fb_h passed explicitly rather than assumed, the same choice text.c's
 * draw_run() makes, so this header carries no dependency on music_hook.c's
 * FB_W/FB_H ordering. */
static inline void draw_icon(uint16_t *fb, int fb_w, int fb_h, int x, int y,
                             const icon_t *ic, uint16_t color) {
    for (int row = 0; row < ic->h; row++) {
        int py = y + row;
        if (py < 0 || py >= fb_h) continue;
        const uint8_t *src = ic->a + (size_t)row * ic->w;
        uint16_t *dst = fb + (size_t)py * fb_w;
        for (int col = 0; col < ic->w; col++) {
            int px = x + col;
            if (px < 0 || px >= fb_w) continue;
            unsigned a = src[col];
            if (a) dst[px] = icon_blend(dst[px], color, a);
        }
    }
}

#endif
