#ifndef AAC_H
#define AAC_H
#include <stdint.h>

/* A thin wrapper over the device's libfdk-aac decoder, shared by the two
 * things that need AAC: M4A files and HLS radio. */
typedef struct aac_dec aac_dec_t;

/* asc/asc_len is the AudioSpecificConfig from an MP4's esds box. Pass NULL to
 * decode a self-describing ADTS stream instead, as found in MPEG-TS. */
aac_dec_t *aac_open(const unsigned char *asc, unsigned asc_len);
void       aac_close(aac_dec_t *d);

/* Hand in one access unit; PCM comes back in *out (interleaved s16).
 * Returns frames decoded, 0 if the decoder wants more data, -1 on error. */
int  aac_decode(aac_dec_t *d, const unsigned char *in, unsigned in_len,
                short *out, unsigned out_frames);
/* Streaming ADTS. aac_fill hands bytes over and returns how many the decoder
 * took; the rest must be offered again. aac_frame then yields one frame at a
 * time until it returns 0, meaning it wants more input. */
int  aac_fill(aac_dec_t *d, const unsigned char *in, unsigned in_len);
int  aac_frame(aac_dec_t *d, short *out, unsigned out_frames);

int  aac_rate(const aac_dec_t *d);
int  aac_channels(const aac_dec_t *d);
#endif
