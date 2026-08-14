/* opus_dec.h — Ogg Opus playback via the R1's own /usr/lib/libopus.so,
 * dlopen'd for the same reason as vorbis_dec.h. OpusDecoder is opaque in
 * libopus's own public API (no struct layout to match, unlike vorbis_info),
 * so this is simpler than the Vorbis side -- the only hand-written parsing
 * is the OpusHead identification packet, a small fixed binary layout
 * (RFC 7845) that isn't part of libopus itself. */
#ifndef OPUS_DEC_H
#define OPUS_DEC_H

#include <stdint.h>
#include "ogg_io.h"

/* Opus always runs its internal frame timing at one of a few fixed rates;
 * decoding at the max means never discarding resolution the file offers. */
#define OPUS_DEC_RATE 48000
/* Largest single Opus frame is 120 ms at 48 kHz. */
#define OPUS_DEC_MAX_FRAME (48000 * 120 / 1000)

typedef struct OpusDecoder OpusDecoder;

typedef struct {
    ogg_io_t ogg;
    OpusDecoder *dec;
    int      channels;
    unsigned rate;            /* always OPUS_DEC_RATE */
    uint64_t total_frames;    /* 0 when the last page's granulepos is unknown */
    int      pre_skip;        /* samples to discard at the very start */
    int      skipped;         /* how much of pre_skip has been discarded so far */
    int      eof;

    /* One decoded Opus packet's worth of PCM, held across calls -- a caller
     * asking for fewer frames than one packet decodes to (the common case:
     * a 20 ms packet is 960 frames at 48 kHz, well above most read chunk
     * sizes) needs the remainder kept, not dropped. */
    short    pending[OPUS_DEC_MAX_FRAME * 2];   /* stereo max */
    int      pending_off;
    int      pending_n;
} opus_dec_t;

int      opus_dec_open(opus_dec_t *o, const char *path);
uint64_t opus_dec_read(opus_dec_t *o, short *out, uint64_t want_frames);
/* Coarse, same rounding note as vorbis_dec_seek. */
void     opus_dec_seek(opus_dec_t *o, uint64_t frame);
void     opus_dec_close(opus_dec_t *o);

#endif
