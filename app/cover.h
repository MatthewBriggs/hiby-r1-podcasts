/* cover.h — feed cover art as an RGB565 square, or NULL. Caller frees. */
#ifndef COVER_H
#define COVER_H
#include <stdint.h>
uint16_t *cover_load(const char *jpeg_path, int px);
#endif
