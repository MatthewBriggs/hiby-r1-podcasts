/* playlist.c — M3U playlists on the card.
 *
 * Plain .m3u files in one folder, holding absolute paths, because that is what
 * everything else in the world reads and writes. Nothing here touches the
 * stock player's own database: that file belongs to the scanner, and a
 * playlist the user can also edit on a computer is more useful than one locked
 * inside it.
 *
 * A "Favourites" list is created on first use so that adding a track always
 * has somewhere to go, rather than requiring a text editor before the feature
 * does anything.
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "playlist.h"

#define PL_DIR "/data/mnt/sd_0/Playlists"

static void ensure_dir(void) {
    mkdir(PL_DIR, 0777);
    char fav[LIB_PATH_LEN];
    snprintf(fav, sizeof(fav), "%s/Favourites.m3u", PL_DIR);
    struct stat st;
    if (stat(fav, &st) != 0) {
        FILE *f = fopen(fav, "w");
        if (f) { fputs("#EXTM3U\n", f); fclose(f); }
    }
}

static int cmp_name(const void *a, const void *b) {
    return strcasecmp(((const pl_t *)a)->name, ((const pl_t *)b)->name);
}

int pl_list(pl_t *out, int max) {
    ensure_dir();
    DIR *d = opendir(PL_DIR);
    if (!d) return 0;
    struct dirent *e;
    int n = 0;
    while (n < max && (e = readdir(d))) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcasecmp(dot, ".m3u") != 0) continue;
        snprintf(out[n].path, sizeof(out[n].path), "%s/%s", PL_DIR, e->d_name);
        size_t len = (size_t)(dot - e->d_name);
        if (len >= sizeof(out[n].name)) len = sizeof(out[n].name) - 1;
        memcpy(out[n].name, e->d_name, len);
        out[n].name[len] = '\0';
        n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof(out[0]), cmp_name);
    return n;
}

int pl_read(const char *file, char (*paths)[LIB_PATH_LEN], int max) {
    FILE *f = fopen(file, "r");
    if (!f) return 0;
    char line[LIB_PATH_LEN];
    int n = 0;
    while (n < max && fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (!len || line[0] == '#') continue;       /* blank, or an EXTINF tag */
        snprintf(paths[n], LIB_PATH_LEN, "%s", line);
        n++;
    }
    fclose(f);
    return n;
}

int pl_append(const char *file, const char *track_path) {
    /* Silently adding a second copy is worse than doing nothing visible. */
    char (*have)[LIB_PATH_LEN] = malloc(sizeof(*have) * 512);
    if (have) {
        int n = pl_read(file, have, 512);
        for (int i = 0; i < n; i++)
            if (!strcmp(have[i], track_path)) { free(have); return 0; }
        free(have);
    }
    FILE *f = fopen(file, "a");
    if (!f) return -1;
    fprintf(f, "%s\n", track_path);
    fclose(f);
    return 1;
}
