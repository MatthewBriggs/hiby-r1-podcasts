/* vorbis_dec.h — Ogg Vorbis playback via the R1's own /usr/lib/libvorbis.so,
 * dlopen'd rather than linked (same reasoning as libjpeg in cover.c and ALSA
 * in audio.c: a missing/mismatched library degrades to "can't play this"
 * instead of the app failing to load at all). See NOTES on the repurposing
 * decision -- the stock firmware already ships a full decode-capable build
 * of this library for its own Ogg support, so there is nothing to vendor or
 * cross-compile here beyond the Ogg container demuxer (ogg_io.h) and the
 * public struct layout this needs to match (vendor/vorbis/codec.h, Xiph's
 * own header, BSD licensed). */
#ifndef VORBIS_DEC_H
#define VORBIS_DEC_H

#include <stdint.h>
#include "ogg_io.h"
#include "vendor/vorbis/codec.h"

typedef struct {
    ogg_io_t ogg;
    vorbis_info      vi;
    vorbis_comment   vc;
    vorbis_dsp_state vd;
    vorbis_block     vb;
    int have_dsp;         /* vd/vb are initialized and need clearing */

    int      channels;
    unsigned rate;
    uint64_t total_frames;   /* 0 when the last page's granulepos is unknown */

    /* Samples already decoded by vorbis_synthesis_pcmout() but not yet
     * handed to a caller -- read() can be asked for fewer frames than one
     * decode pass produces. */
    float **pending;
    int     pending_off;
    int     pending_n;
    int     eof;
} vorbis_dec_t;

/* Loads libvorbis.so on first use; returns 0 if the library or the file
 * isn't a valid Vorbis stream. */
int      vorbis_dec_open(vorbis_dec_t *v, const char *path);
uint64_t vorbis_dec_read(vorbis_dec_t *v, short *out, uint64_t want_frames);
/* Coarse: lands on the Ogg page at or before `frame`, not sample-exact --
 * same rounding this app already accepts for M4A (see audio.c's dec_seek). */
void     vorbis_dec_seek(vorbis_dec_t *v, uint64_t frame);
void     vorbis_dec_close(vorbis_dec_t *v);

#endif
