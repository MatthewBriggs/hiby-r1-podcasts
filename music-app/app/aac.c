/* aac.c — AAC decoding through the device's own libfdk-aac.
 *
 * The library is on the device (libfdk-aac.so.2) and is dlopen'd rather than
 * linked, so a firmware without it degrades to "AAC will not play" instead of
 * the whole hook failing to load.
 *
 * The ABI is declared here rather than by vendoring Fraunhofer's headers,
 * which drag in a tree of internal ones. Everything below was taken from the
 * v2.0.1 sources that match the shipped library, not from memory:
 *
 *   - CStreamInfo begins sampleRate, frameSize, numChannels, all INT, so the
 *     three fields this needs sit at offsets 0, 4 and 8. Nothing else in the
 *     struct is touched, so the rest of its layout does not matter.
 *   - TT_MP4_RAW = 0, TT_MP4_ADTS = 2.
 *   - AAC_DEC_OK = 0, AAC_DEC_NOT_ENOUGH_BITS = 0x1002.
 */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aac.h"

#define TT_MP4_RAW   0
#define TT_MP4_ADTS  2
#define AAC_DEC_OK              0
#define AAC_DEC_NOT_ENOUGH_BITS 0x1002

/* Only the leading fields are declared; see the note above. */
typedef struct {
    int sampleRate;
    int frameSize;
    int numChannels;
} fdk_stream_info_t;

static void *g_lib;
static void *(*x_open)(int transport, unsigned nrLayers);
static void  (*x_close)(void *);
static int   (*x_config_raw)(void *, unsigned char *conf[], const unsigned *len);
static int   (*x_fill)(void *, unsigned char *buf[], const unsigned *size, unsigned *valid);
static int   (*x_decode)(void *, short *pcm, const int pcmSize, const unsigned flags);
static fdk_stream_info_t *(*x_info)(void *);

struct aac_dec { void *h; int rate, channels; };

static int load_lib(void) {
    if (g_lib) return 0;
    g_lib = dlopen("libfdk-aac.so.2", RTLD_LAZY);
    if (!g_lib) g_lib = dlopen("libfdk-aac.so", RTLD_LAZY);
    if (!g_lib) return -1;
    *(void **)(&x_open)       = dlsym(g_lib, "aacDecoder_Open");
    *(void **)(&x_close)      = dlsym(g_lib, "aacDecoder_Close");
    *(void **)(&x_config_raw) = dlsym(g_lib, "aacDecoder_ConfigRaw");
    *(void **)(&x_fill)       = dlsym(g_lib, "aacDecoder_Fill");
    *(void **)(&x_decode)     = dlsym(g_lib, "aacDecoder_DecodeFrame");
    *(void **)(&x_info)       = dlsym(g_lib, "aacDecoder_GetStreamInfo");
    if (!x_open || !x_close || !x_config_raw || !x_fill || !x_decode || !x_info) {
        dlclose(g_lib); g_lib = NULL; return -1;
    }
    return 0;
}

aac_dec_t *aac_open(const unsigned char *asc, unsigned asc_len) {
    if (load_lib() != 0) return NULL;
    aac_dec_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->h = x_open(asc ? TT_MP4_RAW : TT_MP4_ADTS, 1);
    if (!d->h) { free(d); return NULL; }
    if (asc && asc_len) {
        unsigned char *p = (unsigned char *)asc;
        unsigned len = asc_len;
        if (x_config_raw(d->h, &p, &len) != AAC_DEC_OK) {
            x_close(d->h); free(d); return NULL;
        }
    }
    return d;
}

void aac_close(aac_dec_t *d) {
    if (!d) return;
    if (d->h) x_close(d->h);
    free(d);
}

int aac_decode(aac_dec_t *d, const unsigned char *in, unsigned in_len,
               short *out, unsigned out_frames) {
    if (!d || !d->h) return -1;
    if (in && in_len) {
        unsigned char *p = (unsigned char *)in;
        unsigned size = in_len, valid = in_len;
        if (x_fill(d->h, &p, &size, &valid) != AAC_DEC_OK) return -1;
    }
    /* pcmSize is counted in samples, not frames: channels are interleaved. */
    int rc = x_decode(d->h, out, (int)(out_frames * 8), 0);
    if (rc == AAC_DEC_NOT_ENOUGH_BITS) return 0;
    if (rc != AAC_DEC_OK) return -1;

    fdk_stream_info_t *si = x_info(d->h);
    if (!si || si->numChannels <= 0) return -1;
    d->rate = si->sampleRate;
    d->channels = si->numChannels;
    return si->frameSize;
}

/* Fill reports, via bytesValid, how much it could NOT take. Ignoring that and
 * assuming the whole buffer was swallowed loses audio at every chunk boundary,
 * which is audible as a tick every few seconds. */
int aac_fill(aac_dec_t *d, const unsigned char *in, unsigned in_len) {
    if (!d || !d->h || !in || !in_len) return 0;
    unsigned char *p = (unsigned char *)in;
    unsigned size = in_len, valid = in_len;
    if (x_fill(d->h, &p, &size, &valid) != AAC_DEC_OK) return -1;
    return (int)(in_len - valid);
}

int aac_frame(aac_dec_t *d, short *out, unsigned out_frames) {
    if (!d || !d->h) return -1;
    int rc = x_decode(d->h, out, (int)(out_frames * 8), 0);
    if (rc == AAC_DEC_NOT_ENOUGH_BITS) return 0;
    if (rc != AAC_DEC_OK) return -1;
    fdk_stream_info_t *si = x_info(d->h);
    if (!si || si->numChannels <= 0) return 0;
    d->rate = si->sampleRate;
    d->channels = si->numChannels;
    return si->frameSize;
}

int aac_rate(const aac_dec_t *d)     { return d ? d->rate : 0; }
int aac_channels(const aac_dec_t *d) { return d ? d->channels : 0; }
