/* text.h — antialiased UTF-8 text from a TrueType font on the device. */
#ifndef TEXT_H
#define TEXT_H

#include <stdint.h>

int  text_init(void);                       /* 0 on success */
int  text_width(const char *s, int px);
/* Draws at (x, y) where y is the top of the line. Returns the end x. */
int  text_draw(uint16_t *fb, int fb_w, int fb_h, int x, int y,
               const char *s, uint16_t colour, int px, int right_edge);

#endif
