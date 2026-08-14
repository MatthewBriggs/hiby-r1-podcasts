#ifndef TS_H
#define TS_H
/* Pull the audio out of one MPEG-TS segment. Returns bytes written to out,
 * which is an ADTS stream ready for the decoder, or -1 if there is no usable
 * audio track. Stateless: HLS segments each carry their own PAT and PMT. */
int ts_extract_audio(const unsigned char *seg, int len,
                     unsigned char *out, int out_max);
#endif
