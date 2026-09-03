#ifndef PLAYLIST_H
#define PLAYLIST_H
#include "library.h"

#define PL_MAX 32
typedef struct {
    char name[LIB_NAME_LEN];     /* shown in the UI, no .m3u */
    char path[LIB_PATH_LEN];     /* the file itself */
} pl_t;

int  pl_list(pl_t *out, int max);                     /* playlists on the card */
int  pl_read(const char *file, char (*paths)[LIB_PATH_LEN], int max);
int  pl_append(const char *file, const char *track_path);
/* R70: rewrites the whole file as "#EXTM3U" + one path per line, in the
 * given order -- reorder/remove both come down to "here is the new full
 * list, write it". Only ever called on a file this app itself created via
 * pl_append()/ensure_dir() (plain #EXTM3U + bare paths, no EXTINF), so
 * there's nothing richer to lose -- see pl_write()'s own comment before
 * pointing this at a playlist some other tool wrote. */
int  pl_write(const char *file, char (*paths)[LIB_PATH_LEN], int n);
#endif
