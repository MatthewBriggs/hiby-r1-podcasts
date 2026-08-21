/* zlbm.c — R33, https://github.com/zipped-album/zlbm. See zlbm.h for v1
 * scope (audio + direct-image cover; no PDF booklet, no XSPF ordering). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "zlbm.h"
#include "vendor/miniz/miniz.h"

#define ZLBM_MAX_TRACKS 256
#define ZLBM_MAX_IMAGES 32

struct zlbm {
    mz_zip_archive zip;
    mz_uint track_idx[ZLBM_MAX_TRACKS];
    int     track_num[ZLBM_MAX_TRACKS];
    int     disc_num[ZLBM_MAX_TRACKS];
    char    track_name[ZLBM_MAX_TRACKS][ZLBM_NAME_LEN];
    int     n_tracks;

    mz_uint image_idx[ZLBM_MAX_IMAGES];
    int     n_images;
};

static void basename_of(const char *path, char *out, size_t n) {
    const char *slash = strrchr(path, '/');
    snprintf(out, n, "%s", slash ? slash + 1 : path);
}

static const char *ext_of(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot ? dot : "";
}

/* Same container set library.c's is_audio() accepts elsewhere in this app —
 * the zlbm spec only asks for FLAC/Opus, but there is no reason to refuse a
 * zlbm someone built with an MP3 track in it. */
static int is_audio_ext(const char *ext) {
    return !strcasecmp(ext, ".flac") || !strcasecmp(ext, ".mp3") ||
           !strcasecmp(ext, ".m4a")  || !strcasecmp(ext, ".wav") ||
           !strcasecmp(ext, ".aiff") || !strcasecmp(ext, ".aif") ||
           !strcasecmp(ext, ".ogg")  || !strcasecmp(ext, ".oga") ||
           !strcasecmp(ext, ".opus");
}

static int is_image_ext(const char *ext) {
    return !strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg") ||
           !strcasecmp(ext, ".png");
}

int zlbm_is_container(const char *path) {
    const char *ext = ext_of(path);
    return !strcasecmp(ext, ".zlbm") || !strcasecmp(ext, ".zip");
}

/* Leading 1-3 digit run, consumed from *p. -1 if the cursor isn't on a digit. */
static int take_num(const char **p) {
    if (!isdigit((unsigned char)**p)) return -1;
    int v = 0, digits = 0;
    while (isdigit((unsigned char)**p) && digits < 3) {
        v = v * 10 + (**p - '0');
        (*p)++;
        digits++;
    }
    return v;
}

/* "01 - Title.flac" -> track 1, disc -1. "2-04 Title.flac" -> disc 2, track
 * 4. Filename-only, since reading real tags here would mean inflating every
 * track just to open zlbm_track_count() — this only has to be good enough
 * to sort a well-named archive; a mis-tagged one still plays, just possibly
 * out of order (matches cmp_track()'s own unnumbered-sinks-last handling). */
static void parse_track_disc(const char *base, int *track, int *disc) {
    *track = -1;
    *disc = -1;
    const char *p = base;
    int a = take_num(&p);
    if (a < 0) return;
    if (*p == '-') {
        const char *p2 = p + 1;
        int b = take_num(&p2);
        if (b >= 0 && (*p2 == ' ' || *p2 == '.' || *p2 == '_' || *p2 == '-')) {
            *disc = a;
            *track = b;
            return;
        }
    }
    if (*p == ' ' || *p == '.' || *p == '_' || *p == '-')
        *track = a;
}

static void parse_track_name(const char *base, char *out, size_t n) {
    const char *p = base;
    while (*p && (isdigit((unsigned char)*p) || *p == '-')) p++;
    while (*p == ' ' || *p == '.' || *p == '_' || *p == '-') p++;
    if (!*p) p = base;
    snprintf(out, n, "%s", p);
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
}

zlbm_t *zlbm_open(const char *path) {
    zlbm_t *z = calloc(1, sizeof(*z));
    if (!z) return NULL;

    if (!mz_zip_reader_init_file(&z->zip, path, 0)) {
        free(z);
        return NULL;
    }

    mz_uint n = mz_zip_reader_get_num_files(&z->zip);
    for (mz_uint i = 0; i < n; i++) {
        if (mz_zip_reader_is_file_a_directory(&z->zip, i)) continue;

        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z->zip, i, &st)) continue;

        char base[ZLBM_NAME_LEN];
        basename_of(st.m_filename, base, sizeof(base));
        /* Hidden dotfiles and macOS zip junk (__MACOSX/._Name) — same class
         * of noise library.c's folder sweep already filters. */
        if (base[0] == '.' || !base[0]) continue;

        const char *ext = ext_of(base);
        if (is_audio_ext(ext) && z->n_tracks < ZLBM_MAX_TRACKS) {
            int t = z->n_tracks++;
            z->track_idx[t] = i;
            parse_track_disc(base, &z->track_num[t], &z->disc_num[t]);
            parse_track_name(base, z->track_name[t], ZLBM_NAME_LEN);
        } else if (is_image_ext(ext) && z->n_images < ZLBM_MAX_IMAGES) {
            z->image_idx[z->n_images++] = i;
        }
        /* PDF booklets and .xspf playlists are recognized by the spec but
         * out of v1 scope (see zlbm.h) — left unclassified, harmlessly. */
    }

    /* Insertion sort by (disc, track, name) — same precedence as library.c's
     * cmp_track(), so a zlbm with disc/track numbers in its filenames sorts
     * exactly like a real album would. Archive order is kept for ties,
     * which for a normally-built zip is already alphabetical. */
    for (int i = 1; i < z->n_tracks; i++) {
        mz_uint idx = z->track_idx[i];
        int trk = z->track_num[i], dsc = z->disc_num[i];
        char name[ZLBM_NAME_LEN];
        memcpy(name, z->track_name[i], sizeof(name));
        int dx = dsc > 0 ? dsc : 1, tx = trk;
        int j = i - 1;
        while (j >= 0) {
            int dy = z->disc_num[j] > 0 ? z->disc_num[j] : 1;
            int ty = z->track_num[j];
            int after = dy < dx || (dy == dx &&
                        ((ty >= 0 && (tx < 0 || ty < tx)) ||
                         (ty < 0 && tx < 0 && strcmp(z->track_name[j], name) < 0)));
            if (after) break;
            z->track_idx[j + 1] = z->track_idx[j];
            z->track_num[j + 1] = z->track_num[j];
            z->disc_num[j + 1] = z->disc_num[j];
            memcpy(z->track_name[j + 1], z->track_name[j], sizeof(name));
            j--;
        }
        z->track_idx[j + 1] = idx;
        z->track_num[j + 1] = trk;
        z->disc_num[j + 1] = dsc;
        memcpy(z->track_name[j + 1], name, sizeof(name));
    }

    return z;
}

void zlbm_close(zlbm_t *z) {
    if (!z) return;
    mz_zip_reader_end(&z->zip);
    free(z);
}

int zlbm_track_count(const zlbm_t *z) { return z ? z->n_tracks : 0; }

void zlbm_track_name(const zlbm_t *z, int i, char *out, size_t n) {
    if (!z || i < 0 || i >= z->n_tracks) { if (n) out[0] = '\0'; return; }
    snprintf(out, n, "%s", z->track_name[i]);
}

int zlbm_track_number(const zlbm_t *z, int i) {
    return (z && i >= 0 && i < z->n_tracks) ? z->track_num[i] : -1;
}

int zlbm_disc_number(const zlbm_t *z, int i) {
    return (z && i >= 0 && i < z->n_tracks) ? z->disc_num[i] : -1;
}

int zlbm_extract_track(const zlbm_t *z, int i, const char *out_path) {
    if (!z || i < 0 || i >= z->n_tracks) return -1;
    zlbm_t *zz = (zlbm_t *)z; /* miniz's reader isn't const-correct */
    return mz_zip_reader_extract_to_file(&zz->zip, z->track_idx[i], out_path, 0) ? 0 : -1;
}

int zlbm_has_cover(const zlbm_t *z) { return z && z->n_images > 0; }

/* zap's own priority (album.py's _sort_images(): "back" sorts last,
 * "front"/"cover"/"folder" sort first) reimplemented as a score, not ported —
 * the technique is the only thing taken from that GPLv3 reference. */
static int cover_score(mz_zip_archive *zip, mz_uint idx) {
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(zip, idx, &st)) return 1;
    char base[ZLBM_NAME_LEN], low[ZLBM_NAME_LEN];
    basename_of(st.m_filename, base, sizeof(base));
    size_t i = 0;
    for (; base[i] && i + 1 < sizeof(low); i++) low[i] = tolower((unsigned char)base[i]);
    low[i] = '\0';
    if (strstr(low, "front") || strstr(low, "cover") || strstr(low, "folder")) return 2;
    if (strstr(low, "back")) return 0;
    return 1;
}

int zlbm_extract_cover(const zlbm_t *z, const char *out_path) {
    if (!z || z->n_images == 0) return -1;
    zlbm_t *zz = (zlbm_t *)z;

    int best = 0, best_score = -1;
    for (int i = 0; i < z->n_images; i++) {
        int s = cover_score(&zz->zip, z->image_idx[i]);
        if (s > best_score) { best_score = s; best = i; }
    }
    return mz_zip_reader_extract_to_file(&zz->zip, z->image_idx[best], out_path, 0) ? 0 : -1;
}
