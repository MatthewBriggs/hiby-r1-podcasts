/* art.c — find a JPEG for a track.
 *
 * Candidates, cheapest first:
 *
 *  1. A picture file in the album folder. Cheapest, and shared by every track
 *     on the album — but only 108 of the 293 album folders here have one under
 *     a conventional name.
 *  2. *Any* other .jpg/.jpeg sitting in that folder. If a single JPEG is in an
 *     album directory it is the cover, whatever it happens to be called;
 *     insisting on cover.jpg/folder.jpg meant a blank panel until someone
 *     renamed files by hand, which is not a reasonable thing to ask of a
 *     library.
 *  3. The picture embedded in the file itself, which is where most of the rest
 *     keep it. FLAC PICTURE blocks and ID3v2 APIC frames are both parsed
 *     directly: they sit at the head of the file, the layouts are a few fixed
 *     fields, and doing it by hand avoids pulling in a tag library for what is
 *     about sixty lines.
 *
 * These are offered one at a time rather than resolved to a single answer,
 * because "a file exists" is not the same as "art that loads": the decoder
 * declines large progressive JPEGs and over-size images by design, and a PNG
 * named .jpg fails outright. Returning the first existing file meant one bad
 * cover.jpg produced a blank panel even when the track carried perfectly good
 * embedded art. The caller walks the list until something actually decodes.
 *
 * Extracted pictures are written once into the cover cache and thereafter hit
 * as ordinary files, so this runs at full cost only the first time an album is
 * opened.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <strings.h>

#include <stdint.h>

#include "art.h"

/* Extracted art is a means to an end: it is decoded immediately and only the
 * scaled bitmap is kept. Writing it to tmpfs rather than storage keeps a
 * 1.5 MB spill per album out of the filesystem entirely. */
#define ART_SCRATCH "/tmp/.music_art.jpg"
#define MAX_PIC (4 * 1024 * 1024)   /* refuse absurd embedded art outright */

static int file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && st.st_size > 0;
}

static void album_dir(const char *track, char *out, size_t n) {
    snprintf(out, n, "%s", track);
    char *slash = strrchr(out, '/');
    if (slash) *slash = '\0';
}

static uint32_t be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* Copy len bytes from the current offset of f into a new JPEG in the cache. */
static int spill(FILE *f, uint32_t len, const char *dst) {
    if (len == 0 || len > MAX_PIC) return -1;
    FILE *o = fopen(dst, "wb");
    if (!o) return -1;
    char buf[8192];
    uint32_t left = len;
    while (left) {
        size_t want = left > sizeof(buf) ? sizeof(buf) : left;
        size_t got = fread(buf, 1, want, f);
        if (got == 0) break;
        fwrite(buf, 1, got, o);
        left -= (uint32_t)got;
    }
    fclose(o);
    if (left) { unlink(dst); return -1; }
    return 0;
}

/* FLAC: "fLaC", then metadata blocks — a byte of (last<<7 | type) and a 24-bit
 * big-endian length. Type 6 is PICTURE. */
static int flac_picture(FILE *f, const char *dst) {
    unsigned char hdr[4];
    if (fread(hdr, 1, 4, f) != 4 || memcmp(hdr, "fLaC", 4)) return -1;
    for (int guard = 0; guard < 64; guard++) {
        unsigned char b[4];
        if (fread(b, 1, 4, f) != 4) return -1;
        int last = b[0] & 0x80, type = b[0] & 0x7F;
        uint32_t len = ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
        if (type == 6) {
            unsigned char t[4];
            if (fread(t, 1, 4, f) != 4) return -1;          /* picture type */
            if (fread(t, 1, 4, f) != 4) return -1;          /* mime length */
            uint32_t mlen = be32(t);
            char mime[64];
            uint32_t take = mlen < sizeof(mime) - 1 ? mlen : sizeof(mime) - 1;
            if (fread(mime, 1, take, f) != take) return -1;
            mime[take] = '\0';
            if (mlen > take && fseek(f, (long)(mlen - take), SEEK_CUR)) return -1;
            if (fread(t, 1, 4, f) != 4) return -1;          /* description */
            if (fseek(f, (long)be32(t), SEEK_CUR)) return -1;
            if (fseek(f, 16, SEEK_CUR)) return -1;          /* w/h/depth/colours */
            if (fread(t, 1, 4, f) != 4) return -1;
            /* PNG art exists in the wild; the decoder here is JPEG only. */
            if (strstr(mime, "jpeg") || strstr(mime, "jpg"))
                return spill(f, be32(t), dst);
            return -1;
        }
        if (last) return -1;
        if (fseek(f, (long)len, SEEK_CUR)) return -1;
    }
    return -1;
}

static uint32_t syncsafe(const unsigned char *p) {
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7) | (p[3] & 0x7F);
}

/* MP3: an ID3v2 tag with an APIC frame. v2.4 sizes are syncsafe, v2.3 are not —
 * getting that wrong walks off the end of the frame and finds nothing. */
static int id3_apic(FILE *f, const char *dst) {
    unsigned char h[10];
    if (fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3)) return -1;
    int ver = h[3];
    uint32_t tag_size = syncsafe(h + 6);
    uint32_t pos = 0;
    while (pos + 10 < tag_size) {
        unsigned char fh[10];
        if (fread(fh, 1, 10, f) != 10) return -1;
        if (fh[0] == 0) return -1;                     /* padding */
        uint32_t fsize = (ver >= 4) ? syncsafe(fh + 4) : be32(fh + 4);
        if (fsize == 0 || fsize > MAX_PIC) return -1;
        if (!memcmp(fh, "APIC", 4)) {
            long end = ftell(f) + (long)fsize;
            int enc = fgetc(f);
            (void)enc;
            char mime[64];
            size_t i = 0;
            int c;
            while ((c = fgetc(f)) > 0 && i < sizeof(mime) - 1) mime[i++] = (char)c;
            mime[i] = '\0';
            fgetc(f);                                  /* picture type */
            while ((c = fgetc(f)) > 0) { }             /* description */
            long data = ftell(f);
            if (strstr(mime, "jpeg") || strstr(mime, "jpg"))
                return spill(f, (uint32_t)(end - data), dst);
            return -1;
        }
        if (fseek(f, (long)fsize, SEEK_CUR)) return -1;
        pos += 10 + fsize;
    }
    return -1;
}

/* MP4 keeps cover art in moov/udta/meta/ilst/covr, several levels down a box
 * tree. This used to scan the first 512 KB for the atom instead of walking
 * there, on the reasoning that moov sits at the front. An .m4b does the
 * opposite — mdat first, moov at the very end — so an audiobook's cover is
 * 600 MB past where that scan looked, and every book drew a blank panel. The
 * descent is only the four containers on the way down, and it reads box
 * headers rather than buffering anything. */
static int mp4_find_covr(FILE *f, long start, long end, int depth,
                         long *out_off, uint32_t *out_len) {
    if (depth > 6) return -1;
    long at = start;
    while (at + 8 <= end) {
        unsigned char h[16];
        if (fseek(f, at, SEEK_SET) != 0 || fread(h, 1, 8, f) != 8) return -1;
        long long sz = be32(h);
        long hdr = 8;
        if (sz == 1) {
            if (fread(h + 8, 1, 8, f) != 8) return -1;
            sz = ((long long)be32(h + 8) << 32) | be32(h + 12);
            hdr = 16;
        } else if (sz == 0) {
            sz = end - at;
        }
        if (sz < hdr) return -1;
        long body = at + hdr, after = at + (long)sz;
        if (after > end) after = end;

        if (!memcmp(h + 4, "covr", 4)) {
            /* covr's payload is a 'data' box: 4 size, 4 type, 4 flags,
             * 4 reserved. The flags say what the image is: 13 JPEG, 14 PNG. */
            unsigned char d[16];
            if (fread(d, 1, 16, f) != 16 || memcmp(d + 4, "data", 4) != 0) return -1;
            uint32_t dsize = be32(d);
            uint32_t flags = be32(d + 8) & 0xFFFFFF;
            if (flags != 13 || dsize <= 16 || dsize > MAX_PIC) return -1;
            *out_off = body + 16;
            *out_len = dsize - 16;
            return 0;
        }
        if (!memcmp(h + 4, "moov", 4) || !memcmp(h + 4, "udta", 4) ||
            !memcmp(h + 4, "ilst", 4)) {
            if (mp4_find_covr(f, body, after, depth + 1, out_off, out_len) == 0) return 0;
        } else if (!memcmp(h + 4, "meta", 4)) {
            /* A FullBox: version and flags sit before its children. */
            if (mp4_find_covr(f, body + 4, after, depth + 1, out_off, out_len) == 0) return 0;
        }
        if (after <= at) return -1;
        at = after;
    }
    return -1;
}

static int mp4_cover(FILE *f, const char *dst) {
    if (fseek(f, 0, SEEK_END) != 0) return -1;
    long end = ftell(f);
    if (end <= 0) return -1;
    long off = 0;
    uint32_t len = 0;
    if (mp4_find_covr(f, 0, end, 0, &off, &len) != 0) return -1;
    if (fseek(f, off, SEEK_SET) != 0) return -1;
    return spill(f, len, dst);
}

#define ART_MAX_FILES 16
#define ART_NAME_LEN  256

static int cmp_names(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

/* Ordered list of art filenames in the album folder: the conventional names
 * first, in their established priority, then anything else ending .jpg/.jpeg,
 * sorted so that "candidate n" means the same thing on every call — readdir
 * order is not guaranteed stable and the caller indexes into this repeatedly.
 *
 * Dotfiles are skipped, which also drops the `._` AppleDouble sidecars a Mac
 * leaves all over this library; they are not images and the scanner ignores
 * them too. */
static int folder_art(const char *dir, char out[][ART_NAME_LEN], int max) {
    static const char *known[] = { "cover.jpg", "folder.jpg", "front.jpg",
                                   "Cover.jpg", "Folder.jpg", "cover.jpeg" };
    int n = 0;
    for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]) && n < max; i++) {
        char p[512];
        snprintf(p, sizeof(p), "%s/%s", dir, known[i]);
        if (file_exists(p)) snprintf(out[n++], ART_NAME_LEN, "%s", known[i]);
    }

    int first_extra = n;
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && n < max) {
            if (e->d_name[0] == '.') continue;
            const char *dot = strrchr(e->d_name, '.');
            if (!dot) continue;
            if (strcasecmp(dot, ".jpg") != 0 && strcasecmp(dot, ".jpeg") != 0) continue;
            int dup = 0;
            for (int i = 0; i < first_extra; i++)
                if (strcmp(out[i], e->d_name) == 0) { dup = 1; break; }
            if (!dup) snprintf(out[n++], ART_NAME_LEN, "%s", e->d_name);
        }
        closedir(d);
    }
    if (n > first_extra)
        qsort(out + first_extra, (size_t)(n - first_extra), ART_NAME_LEN, cmp_names);
    return n;
}

/* Fills `out` with the n-th candidate's path and `key` with the cache key
 * (one key per album folder: one decode serves the whole album, and skipping
 * between its tracks neither re-extracts nor re-decodes). Returns 0 while
 * candidates remain, -1 once the list is exhausted.
 *
 * The embedded scan is deliberately last and only paid for when everything
 * cheaper has failed — it parses the file and spills to /tmp, against a stat
 * each for the folder images above it. */
int art_candidate(const char *track_path, int n, char *out, size_t out_n,
                  char *key, size_t key_n) {
    char dir[512];
    album_dir(track_path, dir, sizeof(dir));
    snprintf(key, key_n, "%s", dir);

    char names[ART_MAX_FILES][ART_NAME_LEN];
    int cnt = folder_art(dir, names, ART_MAX_FILES);
    if (n < cnt) {
        snprintf(out, out_n, "%s/%s", dir, names[n]);
        return 0;
    }
    if (n > cnt) return -1;          /* past the embedded slot as well */

    FILE *f = fopen(track_path, "rb");
    if (!f) return -1;
    int rc = flac_picture(f, ART_SCRATCH);
    if (rc != 0) { rewind(f); rc = id3_apic(f, ART_SCRATCH); }
    if (rc != 0) { rewind(f); rc = mp4_cover(f, ART_SCRATCH); }
    fclose(f);
    if (rc != 0) return -1;
    snprintf(out, out_n, "%s", ART_SCRATCH);
    return 0;
}
