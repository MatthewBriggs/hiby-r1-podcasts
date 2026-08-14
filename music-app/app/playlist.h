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
#endif
