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
#include <unistd.h>

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

/* R70: full rewrite, not an in-place edit -- a reorder moves every line
 * anyway, and a remove is one line shorter, so there is no cheap partial
 * update to make. Deliberately loses any EXTINF/header decoration a
 * playlist might carry if it came from somewhere other than this app's own
 * pl_append() -- those are plain "#EXTM3U" + bare paths already, so this
 * matches their native shape exactly and drops nothing real. Written to a
 * temp file first and renamed over the original, same crash-safety shape
 * pod_sync_feeds_from_settings() already uses for settings.txt. */
int pl_write(const char *file, char (*paths)[LIB_PATH_LEN], int n) {
    char tmp[LIB_PATH_LEN + 8];
    snprintf(tmp, sizeof(tmp), "%s.new", file);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fputs("#EXTM3U\n", f);
    for (int i = 0; i < n; i++) fprintf(f, "%s\n", paths[i]);
    fclose(f);
    if (rename(tmp, file) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* R71: FAT32/exFAT (what the SD card actually is) forbids `/ \ : * ? " < >
 * |` and control characters in a filename, and Windows tools (this app's
 * playlists are plain .m3u specifically so they're readable elsewhere)
 * additionally choke on a trailing dot or space. Replaced with '_' rather
 * than dropped outright -- "AC/DC" collapsing to "ACDC" reads as a typo an
 * unrelated playlist, "AC_DC" still reads as the name that was actually
 * typed. Trailing dots/spaces are trimmed instead, since a trailing
 * underscore for those would look stranger than just not being there. */
static void sanitize_filename(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < outsz; p++) {
        unsigned char c = (unsigned char)*p;
        out[o++] = (c < 0x20 || strchr("/\\:*?\"<>|", c)) ? '_' : (char)c;
    }
    out[o] = '\0';
    while (o > 0 && (out[o - 1] == '.' || out[o - 1] == ' ')) out[--o] = '\0';
}

int pl_create(const char *name, char *out_path, size_t out_sz) {
    ensure_dir();
    char safe[LIB_NAME_LEN];
    sanitize_filename(name, safe, sizeof(safe));
    if (!safe[0]) snprintf(safe, sizeof(safe), "New Playlist");

    char path[LIB_PATH_LEN];
    struct stat st;
    for (int suffix = 1; suffix < 1000; suffix++) {
        if (suffix == 1) snprintf(path, sizeof(path), "%s/%s.m3u", PL_DIR, safe);
        else             snprintf(path, sizeof(path), "%s/%s (%d).m3u", PL_DIR, safe, suffix);
        if (stat(path, &st) != 0) break;   /* first name not already taken */
    }

    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs("#EXTM3U\n", f);
    fclose(f);
    snprintf(out_path, out_sz, "%s", path);
    return 0;
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
