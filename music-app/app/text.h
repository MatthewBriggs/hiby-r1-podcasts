/* text.h — antialiased UTF-8 text from a TrueType font on the device. */
#ifndef TEXT_H
#define TEXT_H

#include <stdint.h>

int  text_init(void);                       /* 0 on success */
int  text_width(const char *s, int px);
/* Draws at (x, y) where y is the top of the line. Returns the end x.
 * clip_top vertically clips the same way fb_h already clips the bottom, so a
 * row scrolled halfway off the top of a list does not paint over whatever is
 * above it (a header, in practice) — glyphs are clipped pixel-row by
 * pixel-row, same mechanism as the existing fb_h bound. */
int  text_draw(uint16_t *fb, int fb_w, int fb_h, int clip_top, int x, int y,
               const char *s, uint16_t colour, int px, int right_edge);

#endif
