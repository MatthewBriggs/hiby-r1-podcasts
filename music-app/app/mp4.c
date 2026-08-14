/* mp4.c — just enough MP4 to play the audio track of an .m4a or .m4b.
 *
 * An MP4 is a tree of boxes: a 32-bit length, a four-character type, then the
 * payload. The audio lives in mdat as bare AAC access units with no framing of
 * their own, so playing the file means reading the sample tables in moov to
 * learn where every access unit starts and how long it is:
 *
 *   stsd -> mp4a -> esds   the AudioSpecificConfig the decoder is set up with
 *   stsz                   the size of every sample
 *   stsc                   how many samples are in each chunk
 *   stco / co64            where each chunk starts
 *
 * See mp4.h for the two things audiobooks forced on the design — picking the
 * audio track out of a file that has more than one, and not expanding the
 * sample tables into memory.
 *
 * Only what an audio-only file needs is handled. There is no video here, and
 * anything with an unfamiliar layout simply fails to open and is skipped.
 */

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include "mp4.h"

/* Reading an eleven-hour book front to back leaves the whole file in the page
 * cache — 25 MB of it was resident on a 56 MB device. Nothing here is ever
 * re-read, so it is pure cost, and the harm is not the cache itself but what
 * it does to *contiguity*: hiby_player's framebuffer wants an order-11
 * (8 MB) DMA allocation when it starts, and a fragmented zone cannot give it
 * one — the panel then comes up dead. Dropping what is already behind the
 * read cursor keeps the footprint flat. */
#define MP4_FADV_EVERY 512          /* samples between trims, ~12 s of audio */
#define MP4_FADV_KEEP  (4 << 20)    /* leave the last few MB alone */

#ifdef __linux__
static void drop_read_cache(mp4_t *m) {
    long upto = m->cur_off - MP4_FADV_KEEP;
    if (upto <= 0) return;
    posix_fadvise(fileno(m->f), 0, upto, POSIX_FADV_DONTNEED);
}
#else
/* Only so this file still builds on a host for the offline parser tests;
 * macOS spells it F_NOCACHE and the tests do not care either way. */
static void drop_read_cache(mp4_t *m) { (void)m; }
#endif

static unsigned rd32(const unsigned char *p) {
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
           ((unsigned)p[2] << 8) | p[3];
}
static unsigned long long rd64(const unsigned char *p) {
    return ((unsigned long long)rd32(p) << 32) | rd32(p + 4);
}

/* Read `n` bytes from an absolute position, skipping the seek when the file is
 * already there. The tables and the audio are in different parts of the file
 * and are read alternately, so this saves a real seek on the common case of
 * walking either one forwards. */
static size_t rdat(mp4_t *m, long pos, void *buf, size_t n) {
    if (m->fpos != pos) {
        if (fseek(m->f, pos, SEEK_SET) != 0) { m->fpos = -1; return 0; }
        m->fpos = pos;
    }
    size_t got = fread(buf, 1, n, m->f);
    if (m->fpos >= 0) m->fpos += (long)got;
    return got;
}

/* ---- the two windowed tables --------------------------------------------- */

/* stsz: how long sample `i` is. */
static unsigned sample_size(mp4_t *m, unsigned i) {
    if (m->stsz_uniform) return m->stsz_uniform;
    if (i >= m->n_samples) return 0;
    if (!(m->szw_n && i >= m->szw_at && i < m->szw_at + m->szw_n)) {
        unsigned base = i - (i % MP4_SZ_WIN);
        unsigned n = m->n_samples - base;
        if (n > MP4_SZ_WIN) n = MP4_SZ_WIN;
        unsigned char *raw = (unsigned char *)m->szw;
        if (rdat(m, m->stsz_at + (long)base * 4, raw, (size_t)n * 4) != (size_t)n * 4) {
            m->szw_n = 0;
            return 0;
        }
        /* Converted in place: entry k is written back over the same four
         * bytes it was read from, so going forwards is safe. */
        for (unsigned k = 0; k < n; k++) m->szw[k] = rd32(raw + k * 4);
        m->szw_at = base; m->szw_n = n;
    }
    return m->szw[i - m->szw_at];
}

/* stco/co64: where chunk `c` starts. Negative if it cannot be read. */
static long chunk_at(mp4_t *m, unsigned c) {
    if (c >= m->n_chunks) return -1;
    if (!(m->chw_n && c >= m->chw_at && c < m->chw_at + m->chw_n)) {
        unsigned base = c - (c % MP4_CH_WIN);
        unsigned n = m->n_chunks - base;
        if (n > MP4_CH_WIN) n = MP4_CH_WIN;
        unsigned w = m->stco_wide ? 8 : 4;
        unsigned char *raw = (unsigned char *)m->chw;
        if (rdat(m, m->stco_at + (long)base * w, raw, (size_t)n * w) != (size_t)n * w) {
            m->chw_n = 0;
            return -1;
        }
        /* Backwards, because a 4-byte stco entry expands into an 8-byte slot:
         * writing forwards would land on entries not yet read. */
        for (unsigned k = n; k-- > 0; )
            m->chw[k] = m->stco_wide ? (long long)rd64(raw + k * 8)
                                     : (long long)rd32(raw + k * 4);
        m->chw_at = base; m->chw_n = n;
    }
    return (long)m->chw[c - m->chw_at];
}

/* stsc is run-length encoded and tiny (85 runs at worst), so it is the one
 * table held whole. `c` is 0-based; the table's first_chunk is 1-based. */
static unsigned per_chunk(const mp4_t *m, unsigned c) {
    unsigned one = c + 1, per = 0;
    for (unsigned k = 0; k < m->n_stsc; k++) {
        if (m->stsc[k].first_chunk > one) break;
        per = m->stsc[k].per;
    }
    return per;
}

/* Put the cursor on sample `idx`: find the chunk holding it by walking the
 * stsc runs, then add up the sizes of the samples before it in that chunk. */
static int locate(mp4_t *m, unsigned idx) {
    unsigned seen = 0;
    for (unsigned k = 0; k < m->n_stsc; k++) {
        unsigned first = m->stsc[k].first_chunk ? m->stsc[k].first_chunk : 1;
        unsigned last  = (k + 1 < m->n_stsc) ? m->stsc[k + 1].first_chunk
                                             : m->n_chunks + 1;
        if (last > m->n_chunks + 1) last = m->n_chunks + 1;
        if (last <= first) continue;
        unsigned per = m->stsc[k].per;
        unsigned long long run = (unsigned long long)(last - first) * per;
        if (per && idx < seen + run) {
            unsigned within = idx - seen;
            unsigned c  = first - 1 + within / per;
            unsigned in = within % per;
            long off = chunk_at(m, c);
            if (off < 0) return -1;
            for (unsigned i = 0; i < in; i++) {
                unsigned s = sample_size(m, idx - in + i);
                if (!s) return -1;
                off += (long)s;
            }
            m->cur_chunk = c; m->cur_in_chunk = in; m->cur_off = off;
            m->cursor = idx;
            return 0;
        }
        seen += (unsigned)run;
    }
    return -1;
}

/* ---- parsing ------------------------------------------------------------- */

/* esds carries a descriptor chain; the DecoderSpecificInfo (tag 5) inside it
 * is the AudioSpecificConfig. Descriptor lengths are 7 bits per byte with the
 * top bit as a continuation flag. */
static const unsigned char *desc_find(const unsigned char *p, const unsigned char *end,
                                      int want, unsigned *out_len) {
    while (p < end) {
        int tag = *p++;
        unsigned len = 0;
        for (int i = 0; i < 4 && p < end; i++) {
            unsigned b = *p++;
            len = (len << 7) | (b & 0x7F);
            if (!(b & 0x80)) break;
        }
        if (p + len > end) return NULL;
        if (tag == want) { *out_len = len; return p; }
        /* Descriptors that contain others: ES (3) and DecoderConfig (4). */
        if (tag == 3)      p += 3;          /* ES_ID + flags */
        else if (tag == 4) p += 13;         /* object type, buffer, bitrates */
        else               p += len;
    }
    return NULL;
}

/* One track's tables, gathered before we know whether this is the track we
 * want. Adopted into the mp4_t only if it turns out to have an esds. */
typedef struct {
    unsigned char asc[MP4_MAX_ASC];
    unsigned      asc_len;
    int           codec;               /* MP4_CODEC_AAC or MP4_CODEC_ALAC */
    int           is_text;             /* hdlr says this is a text track */
    unsigned      timescale;
    unsigned long duration;
    long          stts_at;
    unsigned      stts_n;
    long          stsz_at;
    unsigned      stsz_uniform, n_samples;
    long          stco_at;
    int           stco_wide;
    unsigned      n_chunks;
    mp4_stsc_t   *stsc;
    unsigned      n_stsc;
} trak_t;

static void parse(mp4_t *m, trak_t *tk, long start, long end, int depth) {
    if (depth > 8) return;
    long at = start;
    while (at + 8 <= end) {
        unsigned char h[16];
        if (rdat(m, at, h, 8) != 8) return;
        long long sz = rd32(h);
        char type[5];
        memcpy(type, h + 4, 4);
        type[4] = '\0';
        long hdr = 8;
        if (sz == 1) {
            if (rdat(m, at + 8, h + 8, 8) != 8) return;
            sz = (long long)rd64(h + 8);
            hdr = 16;
        } else if (sz == 0) {
            sz = end - at;                      /* runs to the end of its parent */
        }
        if (sz < hdr) return;
        if (at + sz > end) sz = end - at;       /* clamp a size that overruns */
        long body = at + hdr, after = at + (long)sz;

        if (!memcmp(type, "moov", 4) || !memcmp(type, "mdia", 4) ||
            !memcmp(type, "minf", 4) || !memcmp(type, "stbl", 4)) {
            parse(m, tk, body, after, depth + 1);
        } else if (!memcmp(type, "trak", 4)) {
            /* Each track is gathered on its own and only adopted if it has an
             * esds — see the header. The first such track wins, so a file
             * with several audio tracks plays the first rather than the last. */
            if (!m->n_samples) {
                trak_t t;
                memset(&t, 0, sizeof(t));
                parse(m, &t, body, after, depth + 1);
                int usable = t.n_samples && t.n_chunks && t.n_stsc && t.stsc;
                int wanted = m->want_text ? t.is_text : (t.asc_len != 0);
                if (usable && wanted) {
                    memcpy(m->asc, t.asc, t.asc_len);
                    m->asc_len      = t.asc_len;
                    m->codec        = t.codec;
                    m->timescale    = t.timescale;
                    m->duration     = t.duration;
                    m->stts_at      = t.stts_at;
                    m->stts_n       = t.stts_n;
                    m->stsz_at      = t.stsz_at;
                    m->stsz_uniform = t.stsz_uniform;
                    m->n_samples    = t.n_samples;
                    m->stco_at      = t.stco_at;
                    m->stco_wide    = t.stco_wide;
                    m->n_chunks     = t.n_chunks;
                    m->stsc         = t.stsc;
                    m->n_stsc       = t.n_stsc;
                } else {
                    free(t.stsc);
                }
            }
        } else if (!memcmp(type, "stsd", 4)) {
            parse(m, tk, body + 8, after, depth + 1);   /* version/flags + count */
        } else if (!memcmp(type, "mp4a", 4)) {
            parse(m, tk, body + 28, after, depth + 1);  /* SampleEntry + audio fields */
        } else if (tk && !memcmp(type, "alac", 4) && (after - body) >= 28) {
            /* ALAC's sample entry nests a second atom also called "alac",
             * holding the ALACSpecificConfig -- unlike mp4a/esds, both
             * levels share the same four-character type, so this reads the
             * child directly (like esds below) rather than recursing, which
             * would just land back in this same branch. The inner atom
             * carries its own 4-byte version/flags before the 24-byte
             * config proper (confirmed against a real file: frameLength
             * showed up 4 bytes later than the config struct alone would
             * put it) -- easy to miss since it isn't in the widely-copied
             * ALACSpecificConfig struct definition itself. */
            unsigned char h2[8];
            long child = body + 28;
            if (rdat(m, child, h2, 8) == 8 && !memcmp(h2 + 4, "alac", 4)) {
                unsigned char cfg[24];
                if (rdat(m, child + 12, cfg, 24) == 24) {
                    memcpy(tk->asc, cfg, 24);
                    tk->asc_len = 24;
                    tk->codec = MP4_CODEC_ALAC;
                }
            }
        } else if (tk && !memcmp(type, "esds", 4)) {
            unsigned char b[512];
            long n = after - body;
            if (n > (long)sizeof(b)) n = (long)sizeof(b);
            if (n > 4 && rdat(m, body, b, (size_t)n) == (size_t)n) {
                unsigned len = 0;
                const unsigned char *asc = desc_find(b + 4, b + n, 5, &len);
                if (asc && len && len <= MP4_MAX_ASC) {
                    memcpy(tk->asc, asc, len);
                    tk->asc_len = len;
                }
            }
        } else if (tk && !memcmp(type, "mdhd", 4)) {
            unsigned char b[32];
            long n = after - body;
            if (n > (long)sizeof(b)) n = (long)sizeof(b);
            if (n > 0 && rdat(m, body, b, (size_t)n) == (size_t)n) {
                if (b[0] == 0 && n >= 20) {
                    tk->timescale = rd32(b + 12);
                    tk->duration  = rd32(b + 16);
                } else if (b[0] == 1 && n >= 32) {
                    tk->timescale = rd32(b + 20);
                    tk->duration  = (unsigned long)rd64(b + 24);
                }
            }
        } else if (tk && !memcmp(type, "hdlr", 4)) {
            /* version/flags, pre_defined, then the four-character handler. */
            unsigned char b[12];
            if (rdat(m, body, b, 12) == 12 && !memcmp(b + 8, "text", 4))
                tk->is_text = 1;
        } else if (tk && !memcmp(type, "stts", 4)) {
            unsigned char b[8];
            if (rdat(m, body, b, 8) == 8) {
                tk->stts_n  = rd32(b + 4);
                if (tk->stts_n > MP4_MAX_STTS) tk->stts_n = MP4_MAX_STTS;
                tk->stts_at = body + 8;
            }
        } else if (tk && !memcmp(type, "stsz", 4)) {
            unsigned char b[12];
            if (rdat(m, body, b, 12) == 12) {
                tk->stsz_uniform = rd32(b + 4);
                tk->n_samples    = rd32(b + 8);
                tk->stsz_at      = body + 12;
            }
        } else if (tk && (!memcmp(type, "stco", 4) || !memcmp(type, "co64", 4))) {
            unsigned char b[8];
            if (rdat(m, body, b, 8) == 8) {
                tk->stco_wide = type[0] == 'c';   /* co64, not stco */
                tk->n_chunks  = rd32(b + 4);
                tk->stco_at   = body + 8;
            }
        } else if (tk && !memcmp(type, "stsc", 4)) {
            unsigned char b[8];
            if (rdat(m, body, b, 8) == 8) {
                unsigned n = rd32(b + 4);
                if (n > MP4_MAX_STSC) n = MP4_MAX_STSC;
                free(tk->stsc);
                tk->stsc = NULL;
                tk->n_stsc = 0;
                if (n && (tk->stsc = malloc(sizeof(*tk->stsc) * n)) != NULL) {
                    unsigned got = 0;
                    for (unsigned i = 0; i < n; i++) {
                        unsigned char e[12];
                        if (rdat(m, body + 8 + (long)i * 12, e, 12) != 12) break;
                        tk->stsc[i].first_chunk = rd32(e);
                        tk->stsc[i].per         = rd32(e + 4);
                        got++;
                    }
                    tk->n_stsc = got;
                }
            }
        }

        if (after <= at) return;              /* a zero-length box would spin */
        at = after;
    }
}

/* ---- public -------------------------------------------------------------- */

static int mp4_open_kind(mp4_t *m, const char *path, int want_text) {
    memset(m, 0, sizeof(*m));
    m->fpos = -1;
    m->want_text = want_text;
    m->f = fopen(path, "rb");
    if (!m->f) return -1;

    if (fseek(m->f, 0, SEEK_END) != 0) { mp4_close(m); return -1; }
    long end = ftell(m->f);
    m->fpos = end;
    if (end <= 0) { mp4_close(m); return -1; }

    parse(m, NULL, 0, end, 0);

    if (!m->n_samples || !m->n_chunks || !m->n_stsc || locate(m, 0) != 0 ||
        (!want_text && !m->asc_len)) {
        mp4_close(m);
        return -1;
    }
    return 0;
}

int mp4_open(mp4_t *m, const char *path) { return mp4_open_kind(m, path, 0); }

void mp4_close(mp4_t *m) {
    if (m->f) fclose(m->f);
    free(m->stsc);
    memset(m, 0, sizeof(*m));
}

int mp4_next(mp4_t *m, unsigned char *buf, unsigned max) {
    /* A unit that cannot be read is stepped over rather than ending the
     * track: one unreadable access unit in an eleven-hour book should cost a
     * click, not the rest of the chapter. */
    for (int tries = 0; tries < 64 && m->cursor < m->n_samples; tries++) {
        unsigned len = sample_size(m, m->cursor);
        long off = m->cur_off;

        m->cursor++;
        m->cur_in_chunk++;
        m->cur_off += (long)len;
        unsigned per = per_chunk(m, m->cur_chunk);
        if (!per || m->cur_in_chunk >= per) {
            do {
                m->cur_chunk++;
            } while (m->cur_chunk < m->n_chunks && per_chunk(m, m->cur_chunk) == 0);
            m->cur_in_chunk = 0;
            long c = chunk_at(m, m->cur_chunk);
            if (c >= 0) m->cur_off = c;
        }

        if (len && len <= max && rdat(m, off, buf, len) == len) {
            if (++m->fadv_tick >= MP4_FADV_EVERY) {
                m->fadv_tick = 0;
                drop_read_cache(m);
            }
            return (int)len;
        }
    }
    return 0;
}

int mp4_seek(mp4_t *m, unsigned sample_index) {
    if (sample_index > m->n_samples) return -1;
    if (sample_index == m->n_samples) { m->cursor = sample_index; return 0; }
    return locate(m, sample_index);
}

/* A chapter track's samples are its titles: a 16-bit length then the text.
 * Some writers use UTF-16 with a byte-order mark; the panel's font is UTF-8,
 * so those are flattened to their low bytes rather than dropped, which keeps
 * plain English titles readable instead of blank. */
static void chapter_title(const unsigned char *s, int len, char *out, size_t n) {
    out[0] = '\0';
    if (len < 2) return;
    int tlen = (s[0] << 8) | s[1];
    const unsigned char *p = s + 2;
    if (tlen > len - 2) tlen = len - 2;
    if (tlen <= 0) return;

    if (tlen >= 2 && ((p[0] == 0xFE && p[1] == 0xFF) || (p[0] == 0xFF && p[1] == 0xFE))) {
        int big = p[0] == 0xFE;
        size_t o = 0;
        for (int i = 2; i + 1 < tlen && o + 1 < n; i += 2) {
            unsigned c = big ? p[i + 1] : p[i];
            out[o++] = c ? (char)c : ' ';
        }
        out[o] = '\0';
        return;
    }
    size_t take = (size_t)tlen < n - 1 ? (size_t)tlen : n - 1;
    memcpy(out, p, take);
    out[take] = '\0';
}

int mp4_chapters(const char *path, mp4_chapter_t *out, int max) {
    mp4_t m;
    if (mp4_open_kind(&m, path, 1) != 0) return 0;
    if (!m.timescale || !m.stts_n) { mp4_close(&m); return 0; }

    /* stts is (count, delta) runs; a chapter track's are one sample each, but
     * nothing in the format requires that. */
    unsigned long long t = 0;
    unsigned sample = 0;
    int n = 0;
    for (unsigned r = 0; r < m.stts_n && n < max; r++) {
        unsigned char e[8];
        if (rdat(&m, m.stts_at + (long)r * 8, e, 8) != 8) break;
        unsigned cnt = rd32(e), delta = rd32(e + 4);
        for (unsigned k = 0; k < cnt && n < max; k++) {
            unsigned char raw[512];
            out[n].start_ms = (long)(t * 1000ULL / m.timescale);
            out[n].title[0] = '\0';
            if (mp4_seek(&m, sample) == 0) {
                int len = mp4_next(&m, raw, sizeof(raw));
                if (len > 0)
                    chapter_title(raw, len, out[n].title, sizeof(out[n].title));
            }
            if (!out[n].title[0])
                snprintf(out[n].title, sizeof(out[n].title), "Chapter %d", n + 1);
            t += delta;
            sample++;
            n++;
        }
    }
    mp4_close(&m);
    return n;
}

long mp4_duration_ms(const char *path) {
    mp4_t m;
    if (mp4_open(&m, path) != 0) return 0;
    long ms = 0;
    if (m.timescale)
        ms = (long)((unsigned long long)m.duration * 1000ULL / m.timescale);
    mp4_close(&m);
    return ms;
}
