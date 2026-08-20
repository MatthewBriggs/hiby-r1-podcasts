/* tags.c — the track number, read from the file rather than guessed.
 *
 * The number was previously taken from the filename, which works for the half
 * of this library that is named "04 Title" and fails for the half that is not.
 * It matters more than a missing column suggests: it drives the sort order, so
 * a wrong guess does not leave an album unnumbered, it leaves it out of order.
 *
 * Three formats, all read from the head of the file:
 *
 *   FLAC  metadata block type 4, VORBIS_COMMENT, "TRACKNUMBER=7" (or "7/12")
 *   MP4   a 'trkn'/'disk' atom inside ilst, the number in a 16-bit field
 *   MP3   an ID3v2 TRCK/TPOS frame, text, likewise possibly "7/12"
 *
 * Only the first few hundred KB are ever touched; none of these formats put
 * their tags at the end in practice, and reading a whole 13 MB FLAC to find a
 * two-digit number would cost more than the ordering is worth.
 *
 * tag_read() reads track, disc and (FLAC only) title in one file open and one
 * pass over the tag data, rather than three separate opens each re-scanning
 * the same block for a different key — found live scanning a 183-track box
 * set, where the per-open cost is what made opening the album slow. The
 * single-field tag_track_number()/tag_disc_number()/tag_title() wrappers
 * below stay for callers that only want one field.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tags.h"

/* "7" and "7/12" both mean seven — and so does "7" written as UTF-16, which is
 * what ID3v2.4 frequently uses: the text arrives as a BOM followed by digits
 * interleaved with NULs. Skipping those rather than stopping at the first
 * non-digit is the difference between reading the number and reading nothing. */
static int parse_num(const char *s, int len) {
    int v = -1;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x00 || c == 0xFF || c == 0xFE) continue;   /* NUL, or a BOM */
        if (c >= '0' && c <= '9') {
            if (v < 0) v = 0;
            v = v * 10 + (c - '0');
        } else if (v >= 0) {
            break;                                            /* "7/12" -> 7 */
        } else if (c != ' ') {
            break;                                            /* leading junk */
        }
    }
    return v;
}

static uint32_t rd32be(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static uint32_t rd32le(const unsigned char *p) {
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] << 8) | p[0];
}

/* One pass over the VORBIS_COMMENT block, checking every entry against all
 * three keys instead of one -- the whole reason to combine these calls. */
static void flac_all(FILE *f, int *track, int *disc, char *title, unsigned title_n) {
    unsigned char h[4];
    if (fread(h, 1, 4, f) != 4 || memcmp(h, "fLaC", 4)) return;
    for (int guard = 0; guard < 64; guard++) {
        unsigned char b[4];
        if (fread(b, 1, 4, f) != 4) return;
        int last = b[0] & 0x80, type = b[0] & 0x7F;
        uint32_t len = ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
        if (type == 4) {                       /* VORBIS_COMMENT */
            if (len > 1 << 20) return;
            unsigned char *v = malloc(len);
            if (!v) return;
            if (fread(v, 1, len, f) != len) { free(v); return; }
            /* vendor string, then a count, then "KEY=value" entries */
            if (len < 4) { free(v); return; }
            uint32_t off = 4 + rd32le(v);
            if (off + 4 > len) { free(v); return; }
            uint32_t n = rd32le(v + off);
            off += 4;
            for (uint32_t i = 0; i < n && off + 4 <= len; i++) {
                uint32_t clen = rd32le(v + off);
                off += 4;
                if (off + clen > len) break;
                const char *entry = (char *)v + off;
                if (track && *track < 0 && clen > 12 && !strncasecmp(entry, "TRACKNUMBER=", 12))
                    *track = parse_num(entry + 12, (int)clen - 12);
                else if (disc && *disc < 0 && clen > 11 && !strncasecmp(entry, "DISCNUMBER=", 11))
                    *disc = parse_num(entry + 11, (int)clen - 11);
                else if (title && title[0] == '\0' && clen > 6 && !strncasecmp(entry, "TITLE=", 6)) {
                    unsigned take = (unsigned)clen - 6;
                    if (take >= title_n) take = title_n - 1;
                    memcpy(title, entry + 6, take);
                    title[take] = '\0';
                }
                off += clen;
            }
            free(v);
            return;
        }
        if (last) return;
        if (fseek(f, (long)len, SEEK_CUR)) return;
    }
}

/* Scan the head of the file for both atoms in one buffered read, rather than
 * reading the same 256 KB twice for 'trkn' then 'disk'. */
static void mp4_all(FILE *f, int *track, int *disc) {
    static unsigned char buf[256 * 1024];
    size_t n = fread(buf, 1, sizeof(buf), f);
    if (n < 32) return;
    for (size_t i = 0; i + 32 < n; i++) {
        const char *atom = track && *track < 0 && !memcmp(buf + i, "trkn", 4) ? "trkn"
                          : disc  && *disc  < 0 && !memcmp(buf + i, "disk", 4) ? "disk" : NULL;
        if (!atom) continue;
        const unsigned char *d = buf + i + 4;
        if (memcmp(d + 4, "data", 4)) continue;
        uint32_t dsize = rd32be(d);
        if (dsize < 16 || i + 4 + dsize > n) continue;
        /* 16 bytes of data-box header, then 2 reserved, then the number. */
        const unsigned char *p = d + 16;
        int v = (p[2] << 8) | p[3];
        if (v > 0) { if (atom[0] == 't') *track = v; else *disc = v; }
    }
}

static uint32_t syncsafe(const unsigned char *p) {
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7) | (p[3] & 0x7F);
}

/* One frame walk, checking each frame against both TRCK and TPOS. */
static void id3_all(FILE *f, int *track, int *disc) {
    unsigned char h[10];
    if (fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3)) return;
    int ver = h[3];
    uint32_t tag_size = syncsafe(h + 6), pos = 0;
    while (pos + 10 < tag_size) {
        unsigned char fh[10];
        if (fread(fh, 1, 10, f) != 10) return;
        if (fh[0] == 0) return;                    /* padding */
        uint32_t fsize = (ver >= 4) ? syncsafe(fh + 4) : rd32be(fh + 4);
        if (fsize == 0 || fsize > (1 << 16)) return;
        int *out = track && *track < 0 && !memcmp(fh, "TRCK", 4) ? track
                 : disc  && *disc  < 0 && !memcmp(fh, "TPOS", 4) ? disc : NULL;
        if (out) {
            char v[64];
            uint32_t want = fsize < sizeof(v) ? fsize : sizeof(v) - 1;
            if (fread(v, 1, want, f) != want) return;
            v[want] = '\0';
            *out = parse_num(v + 1, (int)want - 1);   /* first byte is text encoding */
        } else if (fseek(f, (long)fsize, SEEK_CUR)) {
            return;
        }
        pos += 10 + fsize;
    }
}

/* Track, disc and (FLAC only -- see the header comment on flac_text's old
 * callers) title, in one open. Any output pointer may be NULL to skip it;
 * track/disc are left at -1 and title untouched when not found or not
 * applicable to the container. title may be NULL/title_n 0 to skip it
 * outright. */
void tag_read(const char *path, int *track, int *disc, char *title, unsigned title_n) {
    if (track) *track = -1;
    if (disc)  *disc  = -1;
    if (title && title_n) title[0] = '\0';
    FILE *f = fopen(path, "rb");
    if (!f) return;
    unsigned char m[12];
    if (fread(m, 1, sizeof(m), f) != sizeof(m)) { fclose(f); return; }
    rewind(f);
    if (!memcmp(m, "fLaC", 4))
        flac_all(f, track, disc, (title && title_n) ? title : NULL, title_n);
    else if (!memcmp(m + 4, "ftyp", 4))
        mp4_all(f, track, disc);
    else if (!memcmp(m, "ID3", 3))
        id3_all(f, track, disc);
    fclose(f);
}

int tag_track_number(const char *path) {
    int v; tag_read(path, &v, NULL, NULL, 0); return v;
}

int tag_disc_number(const char *path) {
    int v; tag_read(path, NULL, &v, NULL, 0); return v;
}

int tag_title(const char *path, char *out, unsigned n) {
    if (!out || n < 2) return -1;
    out[0] = '\0';
    /* Only FLAC ever gets a title from tag_read() (see flac_all) -- MP4/MP3
     * never populate it, matching this function's pre-existing behaviour. */
    tag_read(path, NULL, NULL, out, n);
    return out[0] ? 0 : -1;
}
