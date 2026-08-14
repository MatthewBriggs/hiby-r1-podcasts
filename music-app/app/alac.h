#ifndef ALAC_H
#define ALAC_H
#include <stdint.h>

/* A from-scratch ALAC (Apple Lossless) decoder -- ported from Apple's own
 * reference implementation (github.com/macosforge/alac, Apache License 2.0;
 * see the notice at the top of alac.c). Bundled rather than dlopen'd: unlike
 * libfdk-aac, the device's firmware has no system ALAC library at all. */
typedef struct alac_dec alac_dec_t;

/* cookie/cookie_len is the 24-byte ALACSpecificConfig from an MP4's 'alac'
 * box (see mp4.c's own "alac" handling, which extracts exactly this). */
alac_dec_t *alac_open(const unsigned char *cookie, unsigned cookie_len);
void        alac_close(alac_dec_t *d);

/* Hand in one access unit (one ALAC frame, exactly what mp4_next() returns
 * for an ALAC track); PCM comes back in *out (interleaved s16).
 * Returns frames decoded, or -1 on error. Never 0: unlike AAC, one ALAC
 * frame always decodes to output immediately, no priming needed. */
int alac_decode(alac_dec_t *d, const unsigned char *in, unsigned in_len,
                short *out, unsigned out_frames);

int alac_rate(const alac_dec_t *d);
int alac_channels(const alac_dec_t *d);
#endif
