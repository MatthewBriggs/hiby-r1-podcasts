#ifndef ART_H
#define ART_H
#include <stddef.h>
/* Art candidates for a track, cheapest first: conventional cover filenames,
 * then any other JPEG in the album folder, then the picture embedded in the
 * file itself. Fills `out` with the n-th candidate's path and `key` with the
 * shared per-folder cache key; returns 0 while candidates remain, -1 when the
 * list is exhausted.
 *
 * Offered one at a time on purpose: a file existing is not the same as art
 * that decodes (large progressive JPEGs and over-size images are declined by
 * design), so the caller retries the next candidate rather than showing a
 * blank panel. */
int art_candidate(const char *track_path, int n, char *out, size_t out_n,
                  char *key, size_t key_n);
#endif
