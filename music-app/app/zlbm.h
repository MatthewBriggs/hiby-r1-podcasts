#ifndef ZLBM_H
#define ZLBM_H
#include <stddef.h>

/* R33 — zlbm (zipped-album, https://github.com/zipped-album/zlbm): a plain
 * ZIP holding already-tagged audio (FLAC or Ogg/Opus — both formats this
 * app already decodes), plus optional cover art and, per spec, an optional
 * PDF booklet and an XSPF playlist for custom track order.
 *
 * v1 scope, deliberately: audio tracks + a direct JPEG/PNG cover. PDF-cover
 * extraction needs a real PDF renderer (confirmed by reading zap's own
 * implementation, which leans on PyMuPDF for exactly this) — disproportionate
 * to what it buys here, so a .zlbm with only a PDF booklet gets no cover in
 * v1. XSPF custom ordering is likewise deferred: tag-based ordering (the
 * same track/disc-number sort every other album already uses) covers the
 * common case, and nothing here corrupts a zlbm that happens to carry one. */

#define ZLBM_NAME_LEN 192

typedef struct zlbm zlbm_t;

/* True if the path's extension marks it as a zlbm container ("zlbm" or
 * "zip") — the only thing library code needs to decide whether to hand a
 * path to this module instead of the plain-file path. */
int zlbm_is_container(const char *path);

/* Opens the archive and classifies every entry (audio / image / other).
 * Returns NULL if it isn't a readable ZIP or the central directory is
 * corrupt. Close with zlbm_close() even on later errors. */
zlbm_t *zlbm_open(const char *path);
void    zlbm_close(zlbm_t *z);

int  zlbm_track_count(const zlbm_t *z);
/* Name to display for track i — the tag title when the extracted copy has
 * one, else the entry's filename with the extension stripped. */
void zlbm_track_name(const zlbm_t *z, int i, char *out, size_t n);
/* Track/disc numbers straight from the entry's own tags, -1 when unstated —
 * same fields lib_track_t already carries, so cmp_track()'s existing sort
 * applies unchanged once the caller copies these across. */
int  zlbm_track_number(const zlbm_t *z, int i);
int  zlbm_disc_number(const zlbm_t *z, int i);

/* Inflates track i to `out_path` (any regular file path — a fixed /tmp
 * scratch path, same idea as ART_SCRATCH in art.c) so the existing decoders
 * in audio.c can dec_open() it unmodified. Returns 0 on success. Overwrites
 * out_path if it already exists. */
int zlbm_extract_track(const zlbm_t *z, int i, const char *out_path);

/* True if the archive has at least one direct image (JPEG/PNG) cover
 * candidate — false when the only art is a PDF booklet, which this module
 * does not render. */
int zlbm_has_cover(const zlbm_t *z);
/* Inflates the best cover candidate to out_path. Picks by filename, front
 * over unlabeled over back (zap's own heuristic, reimplemented rather than
 * copied — "back" sorts last, "front"/"cover"/"folder" sort first), falling
 * back to the first image in the archive when nothing matches either. */
int zlbm_extract_cover(const zlbm_t *z, const char *out_path);

#endif
