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
 *   MP4   a 'trkn' atom inside ilst, the number in a 16-bit field
 *   MP3   an ID3v2 TRCK frame, text, likewise possibly "7/12"
 *
 * Only the first few hundred KB are ever touched; none of these formats put
 * their tags at the end in practice, and reading a whole 13 MB FLAC to find a
 * two-digit number would cost more than the ordering is worth.
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

static int flac_field(FILE *f, const char *key) {
    unsigned char h[4];
    if (fread(h, 1, 4, f) != 4 || memcmp(h, "fLaC", 4)) return -1;
    for (int guard = 0; guard < 64; guard++) {
        unsigned char b[4];
        if (fread(b, 1, 4, f) != 4) return -1;
        int last = b[0] & 0x80, type = b[0] & 0x7F;
        uint32_t len = ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
        if (type == 4) {                       /* VORBIS_COMMENT */
            if (len > 1 << 20) return -1;
            unsigned char *v = malloc(len);
            if (!v) return -1;
            if (fread(v, 1, len, f) != len) { free(v); return -1; }
            /* vendor string, then a count, then "KEY=value" entries */
            uint32_t off = 0;
            if (len < 4) { free(v); return -1; }
            uint32_t vlen = rd32le(v);
            off = 4 + vlen;
            if (off + 4 > len) { free(v); return -1; }
            uint32_t n = rd32le(v + off);
            off += 4;
            int found = -1;
            for (uint32_t i = 0; i < n && off + 4 <= len; i++) {
                uint32_t clen = rd32le(v + off);
                off += 4;
                if (off + clen > len) break;
                size_t klen = strlen(key);
                if (clen > klen && !strncasecmp((char *)v + off, key, klen))
                    found = parse_num((char *)v + off + klen, (int)clen - (int)klen);
                off += clen;
                if (found > 0) break;
            }
            free(v);
            return found;
        }
        if (last) return -1;
        if (fseek(f, (long)len, SEEK_CUR)) return -1;
    }
    return -1;
}

/* Scan the head of the file for the atom rather than descending the box tree:
 * one four-character name is not worth a parser, and the same shortcut is
 * already used for cover art. */
static int mp4_field(FILE *f, const char *atom) {
    /* Half the size the cover-art scan uses: this runs once per track when an
     * album is opened, not once per album, so the read cost is multiplied. */
    static unsigned char buf[256 * 1024];
    size_t n = fread(buf, 1, sizeof(buf), f);
    if (n < 32) return -1;
    for (size_t i = 0; i + 32 < n; i++) {
        if (memcmp(buf + i, atom, 4)) continue;
        const unsigned char *d = buf + i + 4;
        if (memcmp(d + 4, "data", 4)) continue;
        uint32_t dsize = rd32be(d);
        if (dsize < 16 || i + 4 + dsize > n) continue;
        /* 16 bytes of data-box header, then 2 reserved, then the number. */
        const unsigned char *p = d + 16;
        int v = (p[2] << 8) | p[3];
        return v > 0 ? v : -1;
    }
    return -1;
}

static uint32_t syncsafe(const unsigned char *p) {
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7) | (p[3] & 0x7F);
}

static int id3_field(FILE *f, const char *frame) {
    unsigned char h[10];
    if (fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3)) return -1;
    int ver = h[3];
    uint32_t tag_size = syncsafe(h + 6), pos = 0;
    while (pos + 10 < tag_size) {
        unsigned char fh[10];
        if (fread(fh, 1, 10, f) != 10) return -1;
        if (fh[0] == 0) return -1;                    /* padding */
        uint32_t fsize = (ver >= 4) ? syncsafe(fh + 4) : rd32be(fh + 4);
        if (fsize == 0 || fsize > (1 << 16)) return -1;
        if (!memcmp(fh, frame, 4)) {
            char v[64];
            uint32_t want = fsize < sizeof(v) ? fsize : sizeof(v) - 1;
            if (fread(v, 1, want, f) != want) return -1;
            v[want] = '\0';
            /* First byte is the text encoding. */
            return parse_num(v + 1, (int)want - 1);
        }
        if (fseek(f, (long)fsize, SEEK_CUR)) return -1;
        pos += 10 + fsize;
    }
    return -1;
}

/* Same three containers, the other field: DISCNUMBER, disk, TPOS. Without it a
 * multi-disc set interleaves — every disc's track 1, then every track 2. */
static int flac_field(FILE *f, const char *key);
static int mp4_field(FILE *f, const char *atom);
static int id3_field(FILE *f, const char *frame);

/* Only FLAC and ID3 carry a title cheaply enough to be worth reading here; an
 * MP4 keeps it deep in the box tree and those files are all indexed anyway. */
static int flac_text(FILE *f, const char *key, char *out, unsigned n) {
    unsigned char h[4];
    if (fread(h, 1, 4, f) != 4 || memcmp(h, "fLaC", 4)) return -1;
    for (int guard = 0; guard < 64; guard++) {
        unsigned char b[4];
        if (fread(b, 1, 4, f) != 4) return -1;
        int last = b[0] & 0x80, type = b[0] & 0x7F;
        uint32_t len = ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
        if (type == 4) {
            if (len > (1u << 20)) return -1;
            unsigned char *v = malloc(len);
            if (!v) return -1;
            if (fread(v, 1, len, f) != len) { free(v); return -1; }
            uint32_t off = 4 + rd32le(v), cnt, i;
            if (off + 4 > len) { free(v); return -1; }
            cnt = rd32le(v + off); off += 4;
            int rc = -1;
            size_t klen = strlen(key);
            for (i = 0; i < cnt && off + 4 <= len; i++) {
                uint32_t clen = rd32le(v + off); off += 4;
                if (off + clen > len) break;
                if (clen > klen && !strncasecmp((char *)v + off, key, klen)) {
                    unsigned take = clen - (unsigned)klen;
                    if (take >= n) take = n - 1;
                    memcpy(out, v + off + klen, take);
                    out[take] = '\0';
                    rc = 0;
                    break;
                }
                off += clen;
            }
            free(v);
            return rc;
        }
        if (last) return -1;
        if (fseek(f, (long)len, SEEK_CUR)) return -1;
    }
    return -1;
}

int tag_title(const char *path, char *out, unsigned n) {
    if (!out || n < 2) return -1;
    out[0] = '\0';
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char m[12];
    int rc = -1;
    if (fread(m, 1, sizeof(m), f) == sizeof(m)) {
        rewind(f);
        if (!memcmp(m, "fLaC", 4)) rc = flac_text(f, "TITLE=", out, n);
    }
    fclose(f);
    return rc;
}

int tag_disc_number(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char m[12];
    if (fread(m, 1, sizeof(m), f) != sizeof(m)) { fclose(f); return -1; }
    rewind(f);
    int v = -1;
    if (!memcmp(m, "fLaC", 4))          v = flac_field(f, "DISCNUMBER=");
    else if (!memcmp(m + 4, "ftyp", 4)) v = mp4_field(f, "disk");
    else if (!memcmp(m, "ID3", 3))      v = id3_field(f, "TPOS");
    fclose(f);
    return v;
}

int tag_track_number(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char m[12];
    if (fread(m, 1, sizeof(m), f) != sizeof(m)) { fclose(f); return -1; }
    rewind(f);

    int v = -1;
    if (!memcmp(m, "fLaC", 4))            v = flac_field(f, "TRACKNUMBER=");
    else if (!memcmp(m + 4, "ftyp", 4))   v = mp4_field(f, "trkn");
    else if (!memcmp(m, "ID3", 3))        v = id3_field(f, "TRCK");
    fclose(f);
    return v;
}
