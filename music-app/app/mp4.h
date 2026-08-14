#ifndef MP4_H
#define MP4_H
#include <stdio.h>

/* Enough of an MP4 reader to play the audio track of an .m4a or .m4b.
 *
 * Two things here are shaped by audiobooks rather than by music files:
 *
 * 1. An .m4b holds *two* tracks — the audio, and a text track carrying the
 *    chapter list. Only the audio one may be read. The rule used is "the
 *    track that has an esds": a text track has none, and every trak's tables
 *    are gathered separately and only adopted once that track proves to be
 *    the audio one. Reading whichever trak came last is how a chapter list's
 *    seventeen text samples came to be handed to the AAC decoder.
 *
 * 2. Sample positions are computed while reading rather than expanded into
 *    per-sample arrays at open. An 11-hour .m4b holds 1.7 million access
 *    units and a 32-hour one 2.5 million; one size plus one offset each is
 *    ~30 MB on a device with 56 MB of RAM. The tables stay on disk and are
 *    read through two small sliding windows — one refill per thousand
 *    samples, roughly twenty seconds of audio.
 *
 * File offsets are plain 32-bit `long` (fseek/ftell), so this tops out just
 * under 2 GB. The largest audiobook seen is 913 MB.
 */
#define MP4_MAX_ASC  64
#define MP4_SZ_WIN   1024   /* sample sizes held at once */
#define MP4_CH_WIN   256    /* chunk offsets held at once */
#define MP4_MAX_STSC 4096   /* sample-to-chunk runs; 85 is the worst seen */
#define MP4_MAX_STTS 1024   /* time-to-sample runs */

/* Which codec's config ended up in asc[] -- an AAC AudioSpecificConfig from
 * esds, or an ALAC magic cookie from an 'alac' box. Same buffer either way;
 * the caller needs to know which decoder to hand it to. */
#define MP4_CODEC_AAC  0
#define MP4_CODEC_ALAC 1

typedef struct { unsigned first_chunk, per; } mp4_stsc_t;

typedef struct {
    FILE          *f;
    unsigned char  asc[MP4_MAX_ASC];   /* AAC ASC or ALAC magic cookie */
    unsigned       asc_len;
    int            codec;              /* MP4_CODEC_AAC or MP4_CODEC_ALAC */
    unsigned       timescale;
    unsigned long  duration;           /* in timescale units */
    unsigned       n_samples;
    unsigned       cursor;

    /* Which track to adopt: the audio one, or the text one holding the
     * chapter list. Set before parsing; see mp4_chapters. */
    int            want_text;

    /* Where the sample tables are, rather than what is in them. */
    long           stts_at;            /* first time-to-sample run */
    unsigned       stts_n;
    long           stsz_at;            /* first size entry */
    unsigned       stsz_uniform;       /* non-zero: every sample is this long */
    long           stco_at;            /* first chunk offset */
    int            stco_wide;          /* co64 rather than stco */
    unsigned       n_chunks;
    mp4_stsc_t    *stsc;
    unsigned       n_stsc;

    /* Sliding windows over those two tables. */
    unsigned       szw[MP4_SZ_WIN]; unsigned szw_at, szw_n;
    long long      chw[MP4_CH_WIN]; unsigned chw_at, chw_n;

    /* Where the cursor is, carried forward a sample at a time. */
    unsigned       cur_chunk, cur_in_chunk;
    long           cur_off;
    long           fpos;               /* where the FILE really is, -1 unknown */
    unsigned       fadv_tick;          /* samples since the page cache was trimmed */
} mp4_t;

int  mp4_open(mp4_t *m, const char *path);
void mp4_close(mp4_t *m);
/* Read the next access unit into buf. Returns its length, 0 at the end. */
int  mp4_next(mp4_t *m, unsigned char *buf, unsigned max);
int  mp4_seek(mp4_t *m, unsigned sample_index);

/* The track length in milliseconds, without keeping the file open — for
 * listing a book's chapters before any of them has been played. 0 when the
 * file cannot be read or does not say. */
long mp4_duration_ms(const char *path);

/* Chapters, from the text track an .m4b carries beside its audio. Each is a
 * title and where it starts in the file; durations are the gaps between them.
 * Returns how many were written, 0 for a file that has no such track. */
#define MP4_CHAP_TITLE 128
typedef struct {
    char title[MP4_CHAP_TITLE];
    long start_ms;
} mp4_chapter_t;

int mp4_chapters(const char *path, mp4_chapter_t *out, int max);
#endif
