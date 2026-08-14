/* vorbis_dec.c — see vorbis_dec.h. */
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include "vorbis_dec.h"

static void *g_lib;
static void       (*x_info_init)(vorbis_info *);
static void       (*x_info_clear)(vorbis_info *);
static void       (*x_comment_init)(vorbis_comment *);
static void       (*x_comment_clear)(vorbis_comment *);
static int         (*x_synth_idheader)(ogg_packet *);
static int         (*x_synth_headerin)(vorbis_info *, vorbis_comment *, ogg_packet *);
static int         (*x_synth_init)(vorbis_dsp_state *, vorbis_info *);
static int         (*x_synth_restart)(vorbis_dsp_state *);
static void        (*x_dsp_clear)(vorbis_dsp_state *);
static int         (*x_block_init)(vorbis_dsp_state *, vorbis_block *);
static int         (*x_block_clear)(vorbis_block *);
static int         (*x_synthesis)(vorbis_block *, ogg_packet *);
static int         (*x_synth_blockin)(vorbis_dsp_state *, vorbis_block *);
static int         (*x_synth_pcmout)(vorbis_dsp_state *, float ***);
static int         (*x_synth_read)(vorbis_dsp_state *, int);

#define SYM(name, dst) do { \
    *(void **)(&dst) = dlsym(g_lib, name); \
    if (!dst) return -1; \
} while (0)

static int load_lib(void) {
    if (g_lib) return 0;
    g_lib = dlopen("libvorbis.so", RTLD_NOW);
    if (!g_lib) g_lib = dlopen("/usr/lib/libvorbis.so", RTLD_NOW);
    if (!g_lib) g_lib = dlopen("libvorbis.so.0", RTLD_NOW);
    if (!g_lib) return -1;
    SYM("vorbis_info_init", x_info_init);
    SYM("vorbis_info_clear", x_info_clear);
    SYM("vorbis_comment_init", x_comment_init);
    SYM("vorbis_comment_clear", x_comment_clear);
    SYM("vorbis_synthesis_idheader", x_synth_idheader);
    SYM("vorbis_synthesis_headerin", x_synth_headerin);
    SYM("vorbis_synthesis_init", x_synth_init);
    SYM("vorbis_synthesis_restart", x_synth_restart);
    SYM("vorbis_dsp_clear", x_dsp_clear);
    SYM("vorbis_block_init", x_block_init);
    SYM("vorbis_block_clear", x_block_clear);
    SYM("vorbis_synthesis", x_synthesis);
    SYM("vorbis_synthesis_blockin", x_synth_blockin);
    SYM("vorbis_synthesis_pcmout", x_synth_pcmout);
    SYM("vorbis_synthesis_read", x_synth_read);
    return 0;
}

/* The three header packets (identification, comment, setup) always come
 * first, in that order, before any audio -- reading them off ogg_io directly
 * is simpler than special-casing packet 0 vs the rest. */
static int read_headers(vorbis_dec_t *v) {
    for (int i = 0; i < 3; i++) {
        ogg_packet op;
        int rc = ogg_io_next_packet(&v->ogg, &op);
        if (rc != 1) return -1;
        if (i == 0 && !x_synth_idheader(&op)) return -1;
        if (x_synth_headerin(&v->vi, &v->vc, &op) < 0) return -1;
    }
    return 0;
}

int vorbis_dec_open(vorbis_dec_t *v, const char *path) {
    memset(v, 0, sizeof(*v));
    if (load_lib() < 0) return -1;
    if (ogg_io_open(&v->ogg, path) < 0) return -1;

    x_info_init(&v->vi);
    x_comment_init(&v->vc);
    if (read_headers(v) < 0) {
        x_comment_clear(&v->vc);
        x_info_clear(&v->vi);
        ogg_io_close(&v->ogg);
        return -1;
    }
    if (x_synth_init(&v->vd, &v->vi) != 0) {
        x_comment_clear(&v->vc);
        x_info_clear(&v->vi);
        ogg_io_close(&v->ogg);
        return -1;
    }
    x_block_init(&v->vd, &v->vb);
    v->have_dsp = 1;
    v->channels = v->vi.channels;
    v->rate = (unsigned)v->vi.rate;

    /* Total duration: the last page's granulepos is the file's total sample
     * count. A short backward scan from EOF, not a full decode -- the same
     * "ask the container, don't decode to find out" principle dr_mp3's own
     * setup in this app already follows. Best-effort: 0 (unknown, same as
     * this app's own MP3 path) if the tail can't be read cleanly. */
    if (v->ogg.file_size > 0) {
        long tail = v->ogg.file_size > 65536 ? v->ogg.file_size - 65536 : 0;
        FILE *fp = fopen(path, "rb");
        if (fp) {
            fseek(fp, tail, SEEK_SET);
            unsigned char buf[65536];
            size_t n = fread(buf, 1, sizeof(buf), fp);
            fclose(fp);
            /* Scan backward for the last "OggS" page header and read its
             * granulepos (offset 6, 8 bytes, little-endian) directly --
             * cheaper than re-driving libogg's state machine for one field. */
            for (long i = (long)n - 27; i >= 0; i--) {
                if (buf[i]=='O'&&buf[i+1]=='g'&&buf[i+2]=='g'&&buf[i+3]=='S') {
                    uint64_t gp = 0;
                    for (int b = 7; b >= 0; b--) gp = (gp << 8) | buf[i + 6 + b];
                    if (gp != (uint64_t)-1) v->total_frames = gp;
                    break;
                }
            }
        }
    }
    return 0;
}

/* Pulls and decodes packets until vorbis_synthesis_pcmout has samples ready,
 * or the stream ends. Returns the number of frames now pending, or 0 at EOF. */
static int refill(vorbis_dec_t *v) {
    for (;;) {
        int n = x_synth_pcmout(&v->vd, &v->pending);
        if (n > 0) { v->pending_off = 0; v->pending_n = n; return n; }
        ogg_packet op;
        int rc = ogg_io_next_packet(&v->ogg, &op);
        if (rc != 1) { v->eof = 1; return 0; }
        if (x_synthesis(&v->vb, &op) == 0)
            x_synth_blockin(&v->vd, &v->vb);
        /* A bad packet (nonzero return) is skipped -- loop asks for pcmout
         * again, which is still 0, so the next iteration just pulls another
         * packet rather than giving up on the whole stream over one frame. */
    }
}

uint64_t vorbis_dec_read(vorbis_dec_t *v, short *out, uint64_t want_frames) {
    if (!v->have_dsp) return 0;
    uint64_t done = 0;
    while (done < want_frames) {
        if (v->pending_off >= v->pending_n) {
            if (v->eof) break;
            if (refill(v) == 0) break;
        }
        int avail = v->pending_n - v->pending_off;
        uint64_t take = (uint64_t)avail < (want_frames - done) ? (uint64_t)avail : (want_frames - done);
        for (uint64_t i = 0; i < take; i++) {
            for (int ch = 0; ch < v->channels; ch++) {
                float s = v->pending[ch][v->pending_off + i];
                if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
                out[(done + i) * (uint64_t)v->channels + ch] = (short)(s * 32767.0f);
            }
        }
        v->pending_off += (int)take;
        if (v->pending_off >= v->pending_n) x_synth_read(&v->vd, v->pending_n);
        done += take;
    }
    return done;
}

void vorbis_dec_seek(vorbis_dec_t *v, uint64_t frame) {
    if (!v->have_dsp) return;
    if (ogg_io_seek(&v->ogg, (int64_t)frame) < 0) return;
    x_synth_restart(&v->vd);
    v->pending_off = v->pending_n = 0;
    v->eof = 0;
}

void vorbis_dec_close(vorbis_dec_t *v) {
    if (v->have_dsp) {
        x_block_clear(&v->vb);
        x_dsp_clear(&v->vd);
        x_comment_clear(&v->vc);
        x_info_clear(&v->vi);
    }
    ogg_io_close(&v->ogg);
    memset(v, 0, sizeof(*v));
}
