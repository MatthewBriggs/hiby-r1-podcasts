/* bmp.h — Windows BMP loader for Rockbox theme assets.
 *
 * Themes ship plain uncompressed BMPs. Everything is converted to the
 * framebuffer's own RGB565 at load time so drawing is a copy, not a
 * conversion, and the alpha plane is kept only when the image actually has
 * one -- the theme backdrops measured here are fully opaque, and paying three
 * bytes per pixel for a 480x800 backdrop that never blends would be 380 KB of
 * nothing.
 */
#ifndef BMP_H
#define BMP_H

#include <stdint.h>

typedef struct {
    int       w, h;
    uint16_t *px;      /* w*h, RGB565 */
    uint8_t  *a;       /* w*h alpha, or NULL when the image is fully opaque */
} bmp_t;

/* 0 on success. Sub-images are a Rockbox convention rather than a file
 * feature: a strip of n frames stacked vertically, so the caller slices by
 * height/n itself. */
int  bmp_load(const char *path, bmp_t *out);
void bmp_free(bmp_t *b);

#endif
