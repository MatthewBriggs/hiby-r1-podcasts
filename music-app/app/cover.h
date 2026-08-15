/* cover.h — feed cover art as an RGB565 square, or NULL. Caller frees. */
#ifndef COVER_H
#define COVER_H
#include <stdint.h>
/* cache_key identifies the artwork independently of where the JPEG happens to
 * live — extracted art is decoded from a scratch file that is the same path
 * every time, so the path cannot be the key. */
uint16_t *cover_load(const char *jpeg_path, const char *cache_key, int px);

/* The cached bitmap for `cache_key`, or NULL if there isn't one. Lets a caller
 * skip finding a JPEG at all when the decoded answer is already on the card —
 * which matters because the cheapest-looking candidate search is not cheap:
 * embedded art has to parse the audio file and spill it to /tmp before
 * cover_load() gets a chance to say "already cached". `dir` is the album
 * folder, or NULL to skip the staleness check; a cache entry older than the
 * folder loses, so dropping a cover.jpg in by hand still takes effect. */
uint16_t *cover_cached(const char *cache_key, const char *dir, int px);
#endif
