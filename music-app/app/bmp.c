/* bmp.c — see bmp.h.
 *
 * Theme files come off the SD card, which makes them untrusted input in
 * exactly the way cover art is: the dimension and size caps below exist for
 * the same reason cover.c has them, because a bad header should cost a
 * missing image and not the player's life.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "bmp.h"

/* A theme asset is at most a full-screen backdrop. The cap is generous enough
 * for anything sane and small enough that the worst case is bounded: at 4
 * bytes a pixel a 2048x2048 claim is 16 MB, which this device does not have. */
#define BMP_MAX_DIM 2048

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint16_t to565(int r, int g, int b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

int bmp_load(const char *path, bmp_t *out) {
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    unsigned char hdr[54];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        hdr[0] != 'B' || hdr[1] != 'M') { fclose(f); return -1; }

    uint32_t data_off = rd32(hdr + 10);
    uint32_t dib      = rd32(hdr + 14);
    int32_t  w        = (int32_t)rd32(hdr + 18);
    int32_t  h        = (int32_t)rd32(hdr + 22);
    uint16_t bpp      = rd16(hdr + 28);
    uint32_t comp     = rd32(hdr + 30);

    /* A negative height means the rows are stored top-down; positive (the
     * usual, and what every theme file here uses) means bottom-up. */
    int flip = 1;
    if (h < 0) { h = -h; flip = 0; }

    if (dib < 40 || w <= 0 || h <= 0 || w > BMP_MAX_DIM || h > BMP_MAX_DIM) {
        fclose(f); return -1;
    }
    /* 24- and 32-bit only. Paletted and RLE BMPs exist but no theme here uses
     * them, and guessing at formats nothing exercises is how silent breakage
     * gets shipped. BI_RGB(0) and BI_BITFIELDS(3) are both plain pixel data
     * for these depths; anything else is compressed. */
    if ((bpp != 24 && bpp != 32) || (comp != 0 && comp != 3)) {
        fclose(f); return -1;
    }

    size_t npx = (size_t)w * (size_t)h;
    size_t src_stride = (((size_t)w * (bpp / 8)) + 3u) & ~3u;   /* rows pad to 4 bytes */
    unsigned char *row = malloc(src_stride);
    uint16_t *px = malloc(npx * sizeof(uint16_t));
    uint8_t  *al = malloc(npx);                                  /* dropped below if unused */
    if (!row || !px || !al) { free(row); free(px); free(al); fclose(f); return -1; }

    if (fseek(f, (long)data_off, SEEK_SET) != 0) {
        free(row); free(px); free(al); fclose(f); return -1;
    }

    int any_alpha = 0;
    for (int y = 0; y < h; y++) {
        if (fread(row, 1, src_stride, f) != src_stride) {
            free(row); free(px); free(al); fclose(f); return -1;
        }
        int dy = flip ? (h - 1 - y) : y;
        uint16_t *drow = px + (size_t)dy * (size_t)w;
        uint8_t  *arow = al + (size_t)dy * (size_t)w;
        for (int x = 0; x < w; x++) {
            const unsigned char *p = row + (size_t)x * (bpp / 8);
            /* BMP byte order is BGR(A). */
            drow[x] = to565(p[2], p[1], p[0]);
            uint8_t a = (bpp == 32) ? p[3] : 0xFF;
            arow[x] = a;
            if (a != 0xFF) any_alpha = 1;
        }
    }
    free(row);
    fclose(f);

    /* Rockbox's own 32-bit assets are frequently fully opaque -- every
     * backdrop measured here is -- so the alpha plane is thrown away unless
     * some pixel actually uses it. Saves a third of the memory and lets the
     * blitter take its straight memcpy path. */
    if (!any_alpha) { free(al); al = NULL; }

    out->w = w;
    out->h = h;
    out->px = px;
    out->a = al;
    return 0;
}

void bmp_free(bmp_t *b) {
    if (!b) return;
    free(b->px);
    free(b->a);
    b->px = NULL;
    b->a = NULL;
    b->w = b->h = 0;
}
