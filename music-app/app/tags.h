#ifndef TAGS_H
#define TAGS_H

/* Track, disc and (FLAC only) title, in one file open -- prefer this over
 * the single-field wrappers below when more than one is needed, since each
 * of those opens the file independently. Any output pointer may be NULL to
 * skip that field; track/disc land at -1 and title is left untouched when
 * not found or not applicable to the container. Pass title_n 0 (or a NULL
 * title) to skip title entirely. */
void tag_read(const char *path, int *track, int *disc, char *title, unsigned title_n);

/* The track number the file itself claims. Returns -1 when it does not say.
 * Handles FLAC VORBIS_COMMENT, MP4 trkn and ID3v2 TRCK. */
int tag_track_number(const char *path);
/* Disc number, for multi-disc sets. -1 when the file does not say. */
int tag_disc_number(const char *path);
/* The title the file states, for tracks the index never saw. 0 on success. */
int tag_title(const char *path, char *out, unsigned n);
#endif
