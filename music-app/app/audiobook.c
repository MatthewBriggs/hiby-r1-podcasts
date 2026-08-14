/* audiobook.c — folder walk under /Audiobooks. See audiobook.h for why this
 * is not the SQL index. */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "audiobook.h"
#include "mp4.h"
#include "tags.h"

/* Same card, same mount point as library.c's SD_ROOT -- not shared via a
 * header because library.c's is file-local and this is the only other place
 * that needs it. */
#define AUDIOBOOK_ROOT "/data/mnt/sd_0/Audiobooks"

static int is_audio_file(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    return !strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".m4a") ||
           !strcasecmp(dot, ".m4b") || !strcasecmp(dot, ".flac") ||
           !strcasecmp(dot, ".wav");
}

/* The MP4 family says how long it is in a small box near the end of the file;
 * everything else would need a decode pass, so it is left unknown and filled
 * in by the decoder once the chapter is played. */
static int is_mp4_file(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".m4b") || !strcasecmp(dot, ".m4a"));
}

static int dir_has_audio(const char *path) {
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *e;
    int have = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !is_audio_file(e->d_name)) continue;
        have = 1;
        break;
    }
    closedir(d);
    return have;
}

static int count_audio(const char *path) {
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !is_audio_file(e->d_name)) continue;
        n++;
    }
    closedir(d);
    return n;
}

static int cmp_book(const void *a, const void *b) {
    return strcasecmp(((const ab_book_t *)a)->title, ((const ab_book_t *)b)->title);
}

/* Depth-capped, not because a real card is expected to nest deeply, but
 * because there is no loop protection otherwise and a card is untrusted
 * input. A folder that has its own audio files AND subfolders is unusual but
 * not wrong -- it is still a book, and the subfolders are still walked. */
static int walk(const char *path, ab_book_t *out, int n, int max, int depth) {
    if (n >= max || depth > 6) return n;

    if (dir_has_audio(path)) {
        ab_book_t *b = &out[n];
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        snprintf(b->title, sizeof(b->title), "%s", base);
        snprintf(b->dir, sizeof(b->dir), "%s", path);
        b->file_count = count_audio(path);
        n++;
    }

    DIR *d = opendir(path);
    if (!d) return n;
    struct dirent *e;
    while (n < max && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char full[AB_PATH_LEN];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        n = walk(full, out, n, max, depth + 1);
    }
    closedir(d);
    return n;
}

int ab_scan_books(ab_book_t *out, int max) {
    int n = walk(AUDIOBOOK_ROOT, out, 0, max, 0);
    qsort(out, (size_t)n, sizeof(out[0]), cmp_book);
    return n;
}

/* "track2" before "track10": compares runs of digits numerically rather than
 * character by character, the same ordering a person expects from a folder
 * of chapter files. */
static int natural_cmp(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            char *ea, *eb;
            long va = strtol(a, &ea, 10);
            long vb = strtol(b, &eb, 10);
            if (va != vb) return va < vb ? -1 : 1;
            a = ea; b = eb;
        } else {
            if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
            a++; b++;
        }
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int cmp_path(const void *a, const void *b) {
    const char *x = (const char *)a, *y = (const char *)b;
    const char *bx = strrchr(x, '/'); bx = bx ? bx + 1 : x;
    const char *by = strrchr(y, '/'); by = by ? by + 1 : y;
    return natural_cmp(bx, by);
}

static void name_of(const char *path, char *out, size_t n) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(out, n, "%s", base);
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
}

/* Scratch for one file's embedded chapter list. Static rather than automatic:
 * this runs on the UI thread and 256 of these is 34 KB, too much to put on a
 * stack. */
static mp4_chapter_t g_embedded[AB_MAX_CHAPTERS];

int ab_open_book(const char *dir, ab_book_data_t *bk) {
    memset(bk, 0, sizeof(*bk));
    snprintf(bk->dir, sizeof(bk->dir), "%s", dir);

    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while (bk->file_n < AB_MAX_FILES && (e = readdir(d))) {
        if (e->d_name[0] == '.' || !is_audio_file(e->d_name)) continue;
        char full[AB_PATH_LEN];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;
        /* Small enough to rule out audio, generous enough not to reject a
         * real file -- the same floor library.c's folder sweep uses. */
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 16 * 1024)
            continue;
        snprintf(bk->files[bk->file_n], AB_PATH_LEN, "%s", full);
        bk->file_n++;
    }
    closedir(d);
    if (!bk->file_n) return 0;
    qsort(bk->files, (size_t)bk->file_n, AB_PATH_LEN, cmp_path);

    /* One long file: split it by the chapter track it carries itself. */
    if (bk->file_n == 1 && is_mp4_file(bk->files[0])) {
        int nc = mp4_chapters(bk->files[0], g_embedded, AB_MAX_CHAPTERS);
        long total = mp4_duration_ms(bk->files[0]);
        if (nc > 1 && total > 0) {
            for (int i = 0; i < nc; i++) {
                ab_chapter_t *c = &bk->chap[i];
                snprintf(c->title, sizeof(c->title), "%s", g_embedded[i].title);
                c->file          = 0;
                c->file_start_ms = g_embedded[i].start_ms;
                c->book_start_ms = g_embedded[i].start_ms;
                c->dur_ms = ((i + 1 < nc) ? g_embedded[i + 1].start_ms : total)
                          - g_embedded[i].start_ms;
                if (c->dur_ms < 0) c->dur_ms = 0;
            }
            bk->chap_n   = nc;
            bk->total_ms = total;
            return bk->chap_n;
        }
    }

    /* Otherwise a chapter is a file. */
    int64_t at = 0;
    for (int i = 0; i < bk->file_n && i < AB_MAX_CHAPTERS; i++) {
        ab_chapter_t *c = &bk->chap[i];
        if (tag_title(bk->files[i], c->title, sizeof(c->title)) != 0 || !c->title[0])
            name_of(bk->files[i], c->title, sizeof(c->title));
        c->file          = i;
        c->file_start_ms = 0;
        c->book_start_ms = at;
        c->dur_ms        = is_mp4_file(bk->files[i])
                         ? (int64_t)mp4_duration_ms(bk->files[i]) : 0;
        at += c->dur_ms;
        bk->chap_n++;
    }
    bk->total_ms = at;
    return bk->chap_n;
}

/* Not named after the book (titles collide, need escaping) or a hash of it
 * (opaque, can't be eyeballed on the card) -- the folder is already the
 * unique, human-readable key, so the position just lives inside it. */
#define AB_POS_NAME ".position"

void ab_save_position(const ab_book_data_t *bk, int file_idx, long file_ms) {
    if (!bk->dir[0] || file_idx < 0 || file_idx >= bk->file_n || file_ms < 0) return;
    char path[AB_PATH_LEN + 16];
    snprintf(path, sizeof(path), "%s/%s", bk->dir, AB_POS_NAME);
    /* A partial write on power loss must not resume into garbage: write to a
     * temp name and rename over the old one, which is atomic on the same
     * filesystem, rather than truncate-then-write the file being read. */
    char tmp[AB_PATH_LEN + 24];
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    int ok = fprintf(f, "%d %ld\n", file_idx, file_ms) > 0;
    if (fclose(f) != 0) ok = 0;
    if (ok) rename(tmp, path);
    else unlink(tmp);
}

int ab_load_position(const char *dir, int *file_idx, long *file_ms) {
    char path[AB_PATH_LEN + 16];
    snprintf(path, sizeof(path), "%s/%s", dir, AB_POS_NAME);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int fi = -1; long ms = -1;
    int got = fscanf(f, "%d %ld", &fi, &ms);
    fclose(f);
    if (got != 2 || fi < 0 || ms < 0) return -1;
    *file_idx = fi;
    *file_ms = ms;
    return 0;
}
