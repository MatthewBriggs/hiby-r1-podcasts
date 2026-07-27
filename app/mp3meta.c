/* mp3meta.c — see mp3meta.h. */

#include <string.h>

#include "mp3meta.h"

static const int BITRATES_V1L3[16] = {
    0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
};
static const int BITRATES_V2L3[16] = {
    0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0
};
static const int RATES[3][3] = {
    { 44100, 48000, 32000 },   /* MPEG1   */
    { 22050, 24000, 16000 },   /* MPEG2   */
    { 11025, 12000,  8000 },   /* MPEG2.5 */
};

static size_t id3v2_size(const uint8_t *d, size_t len) {
    if (len < 10 || memcmp(d, "ID3", 3) != 0) return 0;
    /* Syncsafe: 7 bits per byte. */
    size_t n = ((size_t)(d[6] & 0x7F) << 21) | ((size_t)(d[7] & 0x7F) << 14) |
               ((size_t)(d[8] & 0x7F) << 7)  |  (size_t)(d[9] & 0x7F);
    return n + 10;
}

int mp3_meta_parse(const uint8_t *data, size_t len, mp3_meta_t *m) {
    memset(m, 0, sizeof(*m));
    if (!data || len < 128) return -1;

    size_t off = id3v2_size(data, len);
    /* Resync in case the tag size is off or absent. */
    size_t limit = off + 8192 < len ? off + 8192 : len - 4;
    while (off < limit && !(data[off] == 0xFF && (data[off + 1] & 0xE0) == 0xE0))
        off++;
    if (off >= limit) return -1;
    m->audio_start = off;

    const uint8_t *h = data + off;
    int ver_bits = (h[1] >> 3) & 3;         /* 3=MPEG1, 2=MPEG2, 0=MPEG2.5 */
    int layer    = (h[1] >> 1) & 3;         /* 1 = Layer III */
    int br_idx   = (h[2] >> 4) & 0xF;
    int sr_idx   = (h[2] >> 2) & 3;
    int chan_mode = (h[3] >> 6) & 3;        /* 3 = mono */
    if (layer != 1 || sr_idx == 3 || ver_bits == 1) return -1;

    int ver_row = (ver_bits == 3) ? 0 : (ver_bits == 2 ? 1 : 2);
    m->rate = RATES[ver_row][sr_idx];
    m->channels = (chan_mode == 3) ? 1 : 2;

    int is_v1 = (ver_bits == 3);
    int bitrate = is_v1 ? BITRATES_V1L3[br_idx] : BITRATES_V2L3[br_idx];
    int spf = is_v1 ? 1152 : 576;           /* samples per frame */

    /* Xing/Info sits after the side information, whose size depends on the
     * MPEG version and whether the stream is mono. */
    int side = is_v1 ? (m->channels == 1 ? 17 : 32)
                     : (m->channels == 1 ?  9 : 17);
    size_t xing = off + 4 + (size_t)side;
    m->stream_bytes = len - off;

    if (xing + 16 < len &&
        (memcmp(data + xing, "Xing", 4) == 0 || memcmp(data + xing, "Info", 4) == 0)) {
        const uint8_t *p = data + xing + 4;
        uint32_t flags = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                         ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
        p += 4;
        uint32_t frames = 0;
        if (flags & 1) {
            frames = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                     ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
            p += 4;
        }
        if (flags & 2) {
            uint32_t bytes = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                             ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
            if (bytes) m->stream_bytes = bytes;
            p += 4;
        }
        if ((flags & 4) && p + 100 <= data + len) {
            memcpy(m->toc, p, 100);
            m->have_toc = 1;
            p += 100;
        }
        if (frames && m->rate)
            m->duration_ms = (int)((uint64_t)frames * spf * 1000 / m->rate);
    }

    /* No Xing: assume CBR and derive from the bitrate. */
    if (!m->duration_ms && bitrate > 0)
        m->duration_ms = (int)((uint64_t)(len - off) * 8 / bitrate);

    return 0;
}

/* Nearest frame sync at or after `off`; the decoder needs a frame boundary. */
size_t mp3_resync(const uint8_t *data, size_t len, size_t off) {
    if (off + 4 > len) return len > 4 ? len - 4 : 0;
    size_t limit = (off + 65536 < len - 4) ? off + 65536 : len - 4;
    for (size_t i = off; i < limit; i++)
        if (data[i] == 0xFF && (data[i + 1] & 0xE0) == 0xE0) return i;
    return off;
}

size_t mp3_meta_seek_offset(const mp3_meta_t *m, size_t len, int ms) {
    if (ms <= 0 || m->duration_ms <= 0) return m->audio_start;
    if (ms > m->duration_ms) ms = m->duration_ms;

    double frac = (double)ms / (double)m->duration_ms;
    size_t base = m->audio_start;
    size_t span = m->stream_bytes;
    if (base + span > len) span = len - base;

    size_t off;
    if (m->have_toc) {
        /* TOC[i] is the byte position at i% of the duration, in 1/256ths. */
        double pct = frac * 100.0;
        int i = (int)pct;
        if (i > 99) i = 99;
        double a = m->toc[i];
        double b = (i < 99) ? m->toc[i + 1] : 256.0;
        double interp = a + (b - a) * (pct - i);
        off = base + (size_t)(interp / 256.0 * (double)span);
    } else {
        off = base + (size_t)(frac * (double)span);
    }
    if (off >= len) off = len > 4 ? len - 4 : 0;

    return off;
}
