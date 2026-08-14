/* library.h — browsing the stock media index. See library.c for the traps. */
#ifndef LIBRARY_H
#define LIBRARY_H

#define LIB_NAME_LEN 192
#define LIB_PATH_LEN 384

/* BG15: lib_group() labels a blank album_artist/artist/genre "Unknown" for
 * display (out[n].owner[0] is set nonzero on that row to mark it), but the
 * database field itself is empty/NUL, not the literal text "Unknown". Pass
 * this sentinel — never a byte a real trimmed tag can start with — as the
 * `value` filter to reach that group's albums/tracks; passing the display
 * label "Unknown" back in as if it were real data matches nothing. */
#define LIB_UNKNOWN_MARK "\x01"

typedef struct {
    char name[LIB_NAME_LEN];
    int  count;              /* albums for an artist, tracks for an album */
    char owner[LIB_NAME_LEN];/* album rows carry their album artist */
} lib_row_t;

typedef struct {
    char name[LIB_NAME_LEN];
    char artist[LIB_NAME_LEN];   /* the track's own, for playlists */
    char path[LIB_PATH_LEN];
    int  track;              /* -1 when the file does not state one */
    int  disc;               /* -1 when single-disc, or unstated */
    int  format, bits, rate, bitrate, dur_ms;
} lib_track_t;

int  lib_open(void);
void lib_close(void);

int  lib_tracks_for_album(const char *artist, const char *album,
                          lib_track_t *out, int max);
/* Grouped browsing. `column` is one of "album_artist", "artist", "genre" —
 * checked against a whitelist, since a column name cannot be bound as a
 * parameter and has to be pasted into the SQL. */
int lib_group(const char *column, lib_row_t *out, int max, int offset);
int lib_group_count(const char *column);

/* Albums, either within one value of a facet or the whole library when
 * `column` is NULL. */
int lib_albums(const char *column, const char *value,
               lib_row_t *out, int max, int offset);
int lib_albums_count(const char *column, const char *value);

/* Find one track by the path this app hands out (a real filesystem path, not
 * the index's own volume notation). Returns 0 on success. */
int lib_track_by_path(const char *real, lib_track_t *out);

/* How many entries sort before `letter`, i.e. the scroll offset that puts the
 * first entry beginning with it at the top. */
int lib_group_offset(const char *column, const char *key);
int lib_albums_offset(const char *column, const char *value, const char *key);

/* Offsets for A through Z and '[' (the exclusive upper bound of Z), all
 * derived from one distinct-list scan.  This is the fast-index counterpart to
 * the single-key helpers above: it preserves their SQLite comparison semantics
 * without rescanning the library once for every letter crossed. */
#define LIB_INDEX_LETTERS 27
int lib_letter_offsets(const char *column, const char *value, int albums,
                       int out[LIB_INDEX_LETTERS]);

/* Every two-character prefix present, in sort order — 2 bytes each, second
 * byte lowercased, unterminated. Gathered once per list, never per letter.
 * `albums` picks the album list over the grouped one. */
int lib_prefixes(const char *column, const char *value, int albums,
                 char *out, int max);

const char *lib_format_name(int code);

#endif
