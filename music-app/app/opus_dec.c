/* opus_dec.c — see opus_dec.h. */
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "opus_dec.h"

static void *g_lib;
static OpusDecoder *(*x_create)(int32_t Fs, int channels, int *error);
static int          (*x_decode)(OpusDecoder *st, const unsigned char *data, int32_t len,
                                short *pcm, int frame_size, int decode_fec);
static void          (*x_destroy)(OpusDecoder *st);

static int load_lib(void) {
    if (g_lib) return 0;
    g_lib = dlopen("libopus.so", RTLD_NOW);
    if (!g_lib) g_lib = dlopen("/usr/lib/libopus.so", RTLD_NOW);
    if (!g_lib) g_lib = dlopen("libopus.so.0", RTLD_NOW);
    if (!g_lib) return -1;
    *(void **)(&x_create)  = dlsym(g_lib, "opus_decoder_create");
    *(void **)(&x_decode)  = dlsym(g_lib, "opus_decode");
    *(void **)(&x_destroy) = dlsym(g_lib, "opus_decoder_destroy");
    if (!x_create || !x_decode || !x_destroy) return -1;
    return 0;
}

/* RFC 7845 OpusHead, the Ogg-Opus mapping's identification packet -- not
 * part of libopus, which only decodes audio packets, so this app parses it
 * by hand: magic(8) version(1) channels(1) pre_skip(2 LE) input_rate(4 LE,
 * informational only -- output is always OPUS_DEC_RATE) output_gain(2 LE,
 * not applied here -- 0 on the overwhelming majority of real files, and a
 * wrong-by-a-few-dB level is a smaller problem than the complexity of a
 * correct Q7.8 dB scale applied to every sample) mapping_family(1). Only
 * mapping family 0 (mono/stereo, no channel table) is supported -- the
 * multichannel/ambisonic case this app has no use for. */
static int parse_opus_head(const unsigned char *p, int n, int *channels, int *pre_skip) {
    if (n < 19 || memcmp(p, "OpusHead", 8) != 0) return -1;
    if (p[9] < 1 || p[9] > 2) return -1;   /* channel count: mono or stereo */
    *channels = p[9];
    *pre_skip = p[10] | (p[11] << 8);
    int mapping_family = p[18];
    if (mapping_family != 0) return -1;
    return 0;
}

int opus_dec_open(opus_dec_t *o, const char *path) {
    memset(o, 0, sizeof(*o));
    if (load_lib() < 0) return -1;
    if (ogg_io_open(&o->ogg, path) < 0) return -1;

    ogg_packet head, tags;
    if (ogg_io_next_packet(&o->ogg, &head) != 1) { ogg_io_close(&o->ogg); return -1; }
    if (parse_opus_head(head.packet, (int)head.bytes, &o->channels, &o->pre_skip) < 0) {
        ogg_io_close(&o->ogg);
        return -1;
    }
    /* OpusTags: no fields this app reads, just consume the packet so the
     * next read() call starts on real audio. */
    if (ogg_io_next_packet(&o->ogg, &tags) != 1) { ogg_io_close(&o->ogg); return -1; }

    int err = 0;
    o->dec = x_create(OPUS_DEC_RATE, o->channels, &err);
    if (!o->dec || err != 0) { ogg_io_close(&o->ogg); return -1; }
    o->rate = OPUS_DEC_RATE;
    o->skipped = 0;

    /* Same tail-scan trick as vorbis_dec_open: the last page's granulepos is
     * the total sample count at OPUS_DEC_RATE, already net of pre-skip and
     * the encoder's own end-trim by definition of the Ogg-Opus mapping. */
    if (o->ogg.file_size > 0) {
        long tail = o->ogg.file_size > 65536 ? o->ogg.file_size - 65536 : 0;
        FILE *fp = fopen(path, "rb");
        if (fp) {
            fseek(fp, tail, SEEK_SET);
            unsigned char buf[65536];
            size_t n = fread(buf, 1, sizeof(buf), fp);
            fclose(fp);
            for (long i = (long)n - 27; i >= 0; i--) {
                if (buf[i]=='O'&&buf[i+1]=='g'&&buf[i+2]=='g'&&buf[i+3]=='S') {
                    uint64_t gp = 0;
                    for (int b = 7; b >= 0; b--) gp = (gp << 8) | buf[i + 6 + b];
                    if (gp != (uint64_t)-1) o->total_frames = gp;
                    break;
                }
            }
        }
    }
    return 0;
}

/* Decodes packets into o->pending until one yields real (post-pre-skip)
 * samples, or the stream ends. Returns the frame count now pending. */
static int refill(opus_dec_t *o) {
    for (;;) {
        ogg_packet op;
        int rc = ogg_io_next_packet(&o->ogg, &op);
        if (rc != 1) { o->eof = 1; return 0; }
        int n = x_decode(o->dec, op.packet, (int)op.bytes, o->pending, OPUS_DEC_MAX_FRAME, 0);
        if (n <= 0) continue;   /* a corrupt packet costs one frame, not the stream */

        int start = 0;
        if (o->skipped < o->pre_skip) {
            int skip_now = o->pre_skip - o->skipped;
            if (skip_now > n) skip_now = n;
            o->skipped += skip_now;
            start = skip_now;
        }
        int have = n - start;
        if (have <= 0) continue;   /* entire packet was pre-skip; get the next one */
        if (start > 0)
            memmove(o->pending, o->pending + (size_t)start * (size_t)o->channels,
                    (size_t)have * (size_t)o->channels * sizeof(short));
        o->pending_off = 0;
        o->pending_n = have;
        return have;
    }
}

uint64_t opus_dec_read(opus_dec_t *o, short *out, uint64_t want_frames) {
    if (!o->dec) return 0;
    uint64_t done = 0;
    while (done < want_frames) {
        if (o->pending_off >= o->pending_n) {
            if (o->eof) break;
            if (refill(o) == 0) break;
        }
        int avail = o->pending_n - o->pending_off;
        uint64_t take = (uint64_t)avail < (want_frames - done) ? (uint64_t)avail : (want_frames - done);
        memcpy(out + done * (uint64_t)o->channels,
               o->pending + (size_t)o->pending_off * (size_t)o->channels,
               (size_t)take * (size_t)o->channels * sizeof(short));
        o->pending_off += (int)take;
        done += take;
    }
    return done;
}

void opus_dec_seek(opus_dec_t *o, uint64_t frame) {
    if (!o->dec) return;
    /* Account for pre-skip the same way the granulepos itself does (RFC
     * 7845: granulepos counts from the pre-skipped start), so seeking to
     * frame 0 lands back at the true beginning rather than replaying skip
     * samples as audio. */
    if (ogg_io_seek(&o->ogg, (int64_t)frame + o->pre_skip) < 0) return;
    o->skipped = o->pre_skip;   /* landed past the head; nothing left to skip */
    o->eof = 0;
}

void opus_dec_close(opus_dec_t *o) {
    if (o->dec) x_destroy(o->dec);
    ogg_io_close(&o->ogg);
    memset(o, 0, sizeof(*o));
}
