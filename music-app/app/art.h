#ifndef ART_H
#define ART_H
#include <stddef.h>

/* Slot n held nothing, but later slots may: advance and ask again. Distinct
 * from -1, which means the list really is exhausted. */
#define ART_SKIP 1

/* The UI-facing scratch path every existing caller passes to art_candidate()
 * below -- exposed here (rather than kept private to art.c) so those callers
 * can name it instead of duplicating the literal string. */
#define ART_SCRATCH "/tmp/.music_art.jpg"

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
 * blank panel.
 *
 * scratch: where an embedded picture (slot 0) gets spilled to before
 * decoding. Every UI-facing caller (art_worker/view_art_worker) passes the
 * shared ART_SCRATCH path, matching what cover_load()'s own /tmp/ staleness
 * exception assumes -- but a second caller extracting concurrently onto
 * that same fixed path would race it (one thread's spill() overwriting the
 * file mid-read by another). The scanner's background cover pre-warm pass
 * passes its own distinct path for exactly that reason: it can run at any
 * time relative to whichever UI art loader happens to be active. */
int art_candidate(const char *track_path, int n, char *out, size_t out_n,
                  char *key, size_t key_n, const char *scratch);

/* The folder containing `track` (everything up to the last '/'). Exposed for
 * R23's Last.fm fallback, which needs to know where to save a fetched cover
 * -- the same folder any other candidate above would have looked in. */
void album_dir(const char *track, char *out, size_t n);
#endif
