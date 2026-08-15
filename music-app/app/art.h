#ifndef ART_H
#define ART_H
#include <stddef.h>

/* Slot n held nothing, but later slots may: advance and ask again. Distinct
 * from -1, which means the list really is exhausted. */
#define ART_SKIP 1

/* Art candidates for a track, best first: the picture embedded in the file
 * (the one chosen for this release), then a conventional cover filename in
 * the album folder, then any other JPEG in it. Fills `out` with the n-th
 * candidate's path and `key` with the shared per-folder cache key; returns 0
 * when `out` was filled, ART_SKIP when this slot was empty but the walk
 * should continue, and -1 when the list is exhausted.
 *
 * Offered one at a time on purpose: a file existing is not the same as art
 * that decodes (large progressive JPEGs and over-size images are declined by
 * design), so the caller retries the next candidate rather than showing a
 * blank panel. */
int art_candidate(const char *track_path, int n, char *out, size_t out_n,
                  char *key, size_t key_n);
#endif
