/* mp3meta.h — duration and seek data read from an MP3's headers.
 *
 * minimp3_ex's mp3dec_ex_open computes duration by scanning every frame, which
 * costs ~5 s for a podcast episode on this device and stalls playback start.
 * Everything needed is in the first few hundred bytes: a Xing/Info header gives
 * an exact frame count and a seek table, and a CBR stream can be measured from
 * its bitrate.
 */
#ifndef MP3META_H
#define MP3META_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t   audio_start;    /* first frame, i.e. past any ID3v2 tag */
    int      rate;
    int      channels;
    int      duration_ms;    /* 0 if unknown */
    int      have_toc;
    uint8_t  toc[100];       /* Xing seek table: percent -> 1/256 of the stream */
    size_t   stream_bytes;   /* audio bytes, per Xing if present */
} mp3_meta_t;

/* Fills meta from an mmap'd file. Returns 0 on success. */
int mp3_meta_parse(const uint8_t *data, size_t len, mp3_meta_t *meta);

/* Nearest frame sync at or after off. */
size_t mp3_resync(const uint8_t *data, size_t len, size_t off);

/* Byte offset to seek to for a position in ms, using the TOC when available. */
size_t mp3_meta_seek_offset(const mp3_meta_t *meta, size_t len, int ms);

#endif /* MP3META_H */
