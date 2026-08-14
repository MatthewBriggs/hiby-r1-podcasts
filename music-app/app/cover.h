/* cover.h — feed cover art as an RGB565 square, or NULL. Caller frees. */
#ifndef COVER_H
#define COVER_H
#include <stdint.h>
/* cache_key identifies the artwork independently of where the JPEG happens to
 * live — extracted art is decoded from a scratch file that is the same path
 * every time, so the path cannot be the key. */
uint16_t *cover_load(const char *jpeg_path, const char *cache_key, int px);
#endif
