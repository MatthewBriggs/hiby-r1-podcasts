/* wsola.h — pitch-preserving time stretch for speech.
 *
 * WSOLA (waveform-similarity overlap-add): slide the analysis window to the
 * offset that best matches what was just emitted, then cross-fade. Keeps pitch
 * intact, unlike resampling, which is what makes 1.5x speech listenable.
 *
 * Interleaved S16, 1 or 2 channels. Feed decoded audio in, pull stretched audio
 * out; the FIFO inside absorbs the rate mismatch.
 */
#ifndef WSOLA_H
#define WSOLA_H

#include <stddef.h>
#include <stdint.h>

int    wsola_init(int rate, int channels);
void   wsola_free(void);
void   wsola_set_speed(float speed);      /* 1.0 = passthrough */
float  wsola_speed(void);
void   wsola_reset(void);                 /* drop buffered audio, e.g. on seek */

/* Frames (not samples) of interleaved S16. Returns frames actually accepted. */
size_t wsola_feed(const int16_t *in, size_t frames);
/* Fills up to max_frames; returns frames written. 0 means "feed me more". */
size_t wsola_read(int16_t *out, size_t max_frames);
/* Frames of input still buffered. */
size_t wsola_pending(void);

#endif /* WSOLA_H */
