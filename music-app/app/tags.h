#ifndef TAGS_H
#define TAGS_H
/* The track number the file itself claims. Returns -1 when it does not say.
 * Handles FLAC VORBIS_COMMENT, MP4 trkn and ID3v2 TRCK. */
int tag_track_number(const char *path);
/* Disc number, for multi-disc sets. -1 when the file does not say. */
int tag_disc_number(const char *path);
/* The title the file states, for tracks the index never saw. 0 on success. */
int tag_title(const char *path, char *out, unsigned n);
#endif
