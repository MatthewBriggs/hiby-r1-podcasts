/* alac.c -- ALAC (Apple Lossless) decoder.
 *
 * Ported from Apple's own reference decoder (github.com/macosforge/alac),
 * specifically ag_dec.c (adaptive Golomb-Rice decode), dp_dec.c (dynamic
 * predictor), matrix_dec.c (stereo un-mixing) and the frame-decode loop from
 * ALACDecoder.cpp -- all of which carry this notice in the upstream source:
 *
 *   Copyright (c) 2011 Apple Inc. All rights reserved.
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *       http://www.apache.org/licenses/LICENSE-2.0
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *
 * The bitstream/predictor/Golomb-Rice math below follows that reference
 * closely -- getting the parameter adaptation subtly wrong is exactly the
 * kind of bug that doesn't show up until a specific file hits it, so this
 * stays close to code that's already been through years of real files
 * rather than a reimplementation from the format description. The frame
 * orchestration (alac_decode, element parsing) is a C translation of
 * ALACDecoder::Decode()'s C++; the BitBuffer/dyn_decomp/unpc_block routines
 * are a near-direct port of the C originals, reformatted to this project's
 * style. The output stage is this project's own: the reference decoder
 * supports 16/20/24/32-bit output, this only ever produces s16 (matching
 * every other decoder in this app), so only the 16-bit un-mix path is
 * carried over faithfully. Sources deeper than 16-bit are truncated rather
 * than dithered -- correct audibly, not bit-exact -- since no 24-bit ALAC
 * file has actually been seen on this device yet; see alac.h.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "alac.h"

/* ---- bit reader (ALACBitUtilities.c) -------------------------------------- */

typedef struct { const uint8_t *cur, *end; unsigned bit; } bitbuf_t;

static void bb_init(bitbuf_t *b, const uint8_t *buf, unsigned len) {
    b->cur = buf; b->end = buf + len; b->bit = 0;
}

/* Reads up to 16 bits, MSB first. */
static uint32_t bb_read(bitbuf_t *b, unsigned n) {
    uint32_t v = ((uint32_t)b->cur[0] << 16) | ((uint32_t)b->cur[1] << 8) | (uint32_t)b->cur[2];
    v = (v << b->bit) & 0x00FFFFFFu;
    b->bit += n;
    v >>= (24 - n);
    b->cur += (b->bit >> 3);
    b->bit &= 7;
    return v;
}

static uint32_t bb_read1(bitbuf_t *b) {
    uint32_t v = (b->cur[0] >> (7 - b->bit)) & 1;
    b->bit += 1;
    b->cur += (b->bit >> 3);
    b->bit &= 7;
    return v;
}

static void bb_advance(bitbuf_t *b, unsigned n) {
    b->bit += n;
    b->cur += (b->bit >> 3);
    b->bit &= 7;
}

static void bb_byte_align(bitbuf_t *b) {
    if (b->bit) bb_advance(b, 8 - b->bit);
}

/* ---- adaptive Golomb-Rice decode (ag_dec.c) ------------------------------- */

#define QBSHIFT 9
#define QB (1 << QBSHIFT)
#define MMULSHIFT 2
#define MDENSHIFT (QBSHIFT - MMULSHIFT - 1)
#define MOFF (1 << (MDENSHIFT - 2))
#define BITOFF 24
#define MAX_PREFIX_16 9
#define MAX_DATATYPE_BITS_16 16
#define MAX_PREFIX_32 9
#define N_MAX_MEAN_CLAMP 0xffff
#define N_MEAN_CLAMP_VAL 0xffff

typedef struct { uint32_t mb, mb0, pb, kb, wb; } ag_params_t;

static void ag_set(ag_params_t *p, uint32_t m, uint32_t pb, uint32_t kb) {
    p->mb = p->mb0 = m; p->pb = pb; p->kb = kb; p->wb = (1u << kb) - 1;
}

/* Count leading zeros of a 32-bit value -- used to find the unary prefix
 * length. A single instruction on most CPUs; this project's toolchain
 * exposes it as a GCC/Clang builtin either way. */
static inline int32_t clz32(uint32_t x) { return x ? __builtin_clz(x) : 32; }

static inline int32_t lg3(int32_t x) { return 31 - clz32((uint32_t)(x + 3)); }

static uint32_t read32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Peeks up to 32 bits starting at an arbitrary bit offset from `in`. */
static uint32_t peekbits(const uint8_t *in, uint32_t bitoff, uint32_t n) {
    uint32_t byteoff = bitoff / 8;
    uint32_t load1 = read32(in + byteoff), result;
    if (n + (bitoff & 7) > 32) {
        uint32_t load2 = (uint32_t)in[byteoff + 4];
        result = load1 << (bitoff & 7);
        load2 >>= (8 - (n + (bitoff & 7) - 32));
        result = (result >> (32 - n)) | load2;
    } else {
        result = load1 >> (32 - n - (bitoff & 7));
    }
    if (n != 32) result &= ~(0xFFFFFFFFu << n);
    return result;
}

/* Escape-coded run of zeros ("zero mode" in the reference). */
static int32_t ag_get_zero_run(const uint8_t *in, uint32_t *bitpos, uint32_t m, uint32_t k) {
    uint32_t pos = *bitpos;
    uint32_t stream = read32(in + (pos >> 3)) << (pos & 7);
    uint32_t pre = clz32(~stream);
    uint32_t result;
    if (pre >= MAX_PREFIX_16) {
        pos += MAX_PREFIX_16;
        stream <<= MAX_PREFIX_16;
        result = stream >> (32 - MAX_DATATYPE_BITS_16);
        pos += MAX_DATATYPE_BITS_16;
    } else {
        uint32_t v;
        pos += pre + 1;
        stream <<= pre + 1;
        v = stream >> (32 - k);
        pos += k;
        result = pre * m + v - 1;
        if (v < 2) { result -= (v - 1); pos -= 1; }
    }
    *bitpos = pos;
    return (int32_t)result;
}

/* One Golomb-Rice-coded residual. */
static int32_t ag_get(const uint8_t *in, uint32_t *bitpos, int32_t m, int32_t k, int32_t maxbits) {
    uint32_t pos = *bitpos;
    uint32_t stream = read32(in + (pos >> 3)) << (pos & 7);
    uint32_t pre = clz32(~stream);
    uint32_t result, v;
    if (pre >= MAX_PREFIX_32) {
        result = peekbits(in, pos + MAX_PREFIX_32, (uint32_t)maxbits);
        pos += MAX_PREFIX_32 + (uint32_t)maxbits;
    } else {
        pos += pre + 1;
        result = pre * (uint32_t)m;
        if (k != 1) {
            stream <<= pre + 1;
            v = stream >> (32 - k);
            pos += k - 1;
            if (v >= 2) { result += v - 1; pos += 1; }
        }
    }
    *bitpos = pos;
    return (int32_t)result;
}

/* Decodes numSamples residuals into pc[]. Returns 0 on success, -1 if the
 * bitstream ran out (a corrupt or truncated frame). */
static int ag_decompress(ag_params_t *params, bitbuf_t *bits, int32_t *pc,
                         int32_t num_samples, int32_t max_size) {
    const uint8_t *in = bits->cur;
    uint32_t bitpos = bits->bit, start = bitpos;
    uint32_t maxpos = (uint32_t)(bits->end - bits->cur) * 8 + 32; /* generous; real bound checked by caller */
    uint32_t mb = params->mb0, pb = params->pb, kb = params->kb, wb = params->wb;
    int32_t zmode = 0, c = 0;

    while (c < num_samples) {
        uint32_t m, k, n;
        int32_t del;
        if (bitpos >= maxpos) return -1;

        m = mb >> QBSHIFT;
        k = lg3((int32_t)m);
        if (k > kb) k = kb;
        m = (1u << k) - 1;

        n = (uint32_t)ag_get(in, &bitpos, (int32_t)m, (int32_t)k, max_size);

        {
            uint32_t ndecode = n + (uint32_t)zmode;
            int32_t mult = -((int32_t)(ndecode & 1));
            mult |= 1;
            del = (int32_t)((ndecode + 1) >> 1) * mult;
        }
        pc[c++] = del;

        mb = pb * (n + (uint32_t)zmode) + mb - ((pb * mb) >> QBSHIFT);
        if (n > N_MAX_MEAN_CLAMP) mb = N_MEAN_CLAMP_VAL;
        zmode = 0;

        if (((mb << MMULSHIFT) < QB) && (c < num_samples)) {
            uint32_t mz;
            zmode = 1;
            k = (uint32_t)clz32(mb) - BITOFF + ((mb + MOFF) >> MDENSHIFT);
            mz = ((1u << k) - 1) & wb;
            n = (uint32_t)ag_get_zero_run(in, &bitpos, mz, k);
            if (c + (int32_t)n > num_samples) return -1;
            for (uint32_t j = 0; j < n; j++) pc[c++] = 0;
            if (n >= 65535) zmode = 0;
            mb = 0;
        }
    }

    bb_advance(bits, bitpos - start);
    return 0;
}

/* ---- dynamic predictor (dp_dec.c) ----------------------------------------- */

static inline int32_t sign32(int32_t i) { return (int32_t)(((uint32_t)-i) >> 31) | (i >> 31); }

/* Reconstructs samples from residuals pc[] into out[] (may alias), applying
 * and adapting an order-`active` FIR-style predictor. Faithful port of
 * unpc_block, general case only -- the reference's numactive==4/8 fast
 * paths are a performance optimization for the same math, dropped here for
 * a smaller, easier-to-verify port; the general loop below handles every
 * order correctly, just without their hand-unrolling. */
static void predictor_reconstruct(const int32_t *pc, int32_t *out, int32_t num,
                                  int16_t *coefs, int32_t active,
                                  uint32_t chanbits, uint32_t denshift) {
    uint32_t chanshift = 32 - chanbits;
    int32_t denhalf = active ? (1 << (denshift - 1)) : 0;

    out[0] = pc[0];
    if (active == 0) {
        if (num > 1 && pc != out) memcpy(&out[1], &pc[1], (size_t)(num - 1) * sizeof(int32_t));
        return;
    }
    if (active == 31) {
        int32_t prev = out[0];
        for (int32_t j = 1; j < num; j++) {
            int32_t del = pc[j] + prev;
            prev = (int32_t)((uint32_t)del << chanshift) >> chanshift;
            out[j] = prev;
        }
        return;
    }

    for (int32_t j = 1; j <= active; j++) {
        int32_t del = pc[j] + out[j - 1];
        out[j] = (int32_t)((uint32_t)del << chanshift) >> chanshift;
    }

    for (int32_t j = active + 1; j < num; j++) {
        int32_t top = out[j - active - 1];
        int32_t *pout = out + j - 1;
        int32_t sum = 0;
        for (int32_t k = 0; k < active; k++)
            sum += coefs[k] * (pout[-k] - top);

        int32_t del = pc[j];
        int32_t del0 = del;
        int32_t sg = sign32(del);
        del += top + ((sum + denhalf) >> denshift);
        out[j] = (int32_t)((uint32_t)del << chanshift) >> chanshift;

        if (sg > 0) {
            for (int32_t k = active - 1; k >= 0; k--) {
                int32_t dd = top - pout[-k];
                int32_t sgn = sign32(dd);
                coefs[k] = (int16_t)(coefs[k] - sgn);
                del0 -= (active - k) * ((sgn * dd) >> denshift);
                if (del0 <= 0) break;
            }
        } else if (sg < 0) {
            for (int32_t k = active - 1; k >= 0; k--) {
                int32_t dd = top - pout[-k];
                int32_t sgn = sign32(dd);
                coefs[k] = (int16_t)(coefs[k] + sgn);
                del0 -= (active - k) * ((-sgn * dd) >> denshift);
                if (del0 >= 0) break;
            }
        }
    }
}

/* ---- stereo un-mix, 16-bit output only (matrix_dec.c, trimmed) ----------- */

static void unmix_s16(const int32_t *u, const int32_t *v, short *out, int stride,
                      int32_t n, int32_t mixbits, int32_t mixres) {
    for (int32_t j = 0; j < n; j++) {
        int32_t l, r;
        if (mixres != 0) {
            l = u[j] + v[j] - ((mixres * v[j]) >> mixbits);
            r = l - v[j];
        } else {
            l = u[j]; r = v[j];
        }
        out[0] = (short)l; out[1] = (short)r;
        out += stride;
    }
}

/* ---- frame decode (ALACDecoder::Decode) ----------------------------------- */

enum { ID_SCE = 0, ID_CPE = 1, ID_CCE = 2, ID_LFE = 3, ID_DSE = 4, ID_PCE = 5, ID_FIL = 6, ID_END = 7 };

struct alac_dec {
    uint32_t frame_len;
    uint8_t  bit_depth, pb, mb, kb, channels;
    uint16_t max_run;
    uint32_t rate;
    int32_t *mix_u, *mix_v, *pred;   /* frame_len each */
};

alac_dec_t *alac_open(const unsigned char *cookie, unsigned len) {
    /* Layout per ALACMagicCookieDescription.txt: an optional 'frma' atom,
     * an optional 'alac' atom header (both 12 bytes), then the 24-byte
     * ALACSpecificConfig itself -- mp4.c's own "alac" parsing already
     * strips both wrappers and hands over exactly the 24-byte config, but
     * tolerate the wrapped form too in case a future caller doesn't. */
    if (len >= 12 && (!memcmp(cookie + 4, "frma", 4))) { cookie += 12; len -= 12; }
    if (len >= 12 && (!memcmp(cookie + 4, "alac", 4))) { cookie += 12; len -= 12; }
    if (len < 24) return NULL;

    alac_dec_t *d = (alac_dec_t *)calloc(1, sizeof(*d));
    if (!d) return NULL;

    d->frame_len = read32(cookie);
    d->bit_depth = cookie[5];
    d->pb        = cookie[6];
    d->mb        = cookie[7];
    d->kb        = cookie[8];
    d->channels  = cookie[9];
    d->max_run   = (uint16_t)((cookie[10] << 8) | cookie[11]);
    d->rate      = read32(cookie + 20);

    if (d->channels < 1 || d->channels > 2 || d->frame_len == 0 || d->frame_len > 65536) {
        free(d); return NULL;
    }

    d->mix_u = (int32_t *)malloc(d->frame_len * sizeof(int32_t));
    d->mix_v = (int32_t *)malloc(d->frame_len * sizeof(int32_t));
    d->pred  = (int32_t *)malloc(d->frame_len * sizeof(int32_t));
    if (!d->mix_u || !d->mix_v || !d->pred) {
        free(d->mix_u); free(d->mix_v); free(d->pred); free(d);
        return NULL;
    }
    return d;
}

void alac_close(alac_dec_t *d) {
    if (!d) return;
    free(d->mix_u); free(d->mix_v); free(d->pred);
    free(d);
}

int alac_rate(const alac_dec_t *d)     { return d ? (int)d->rate : 0; }
int alac_channels(const alac_dec_t *d) { return d ? (int)d->channels : 0; }

/* Skips a run of "shift buffer" bits present when bytesShifted != 0 --
 * see the file header: this decoder truncates to 16-bit output rather than
 * reinserting them, so they only need to be skipped to keep the bitstream
 * in sync for whatever comes next. */
static void skip_shift_bits(bitbuf_t *bits, uint32_t count, uint32_t bytes_shifted) {
    bb_advance(bits, count * bytes_shifted * 8);
}

int alac_decode(alac_dec_t *d, const unsigned char *in, unsigned in_len,
                short *out, unsigned out_frames) {
    if (!d || !in || !in_len) return -1;

    bitbuf_t bits;
    bb_init(&bits, in, in_len);

    /* A frame is all-or-nothing: the sample count drives how the Golomb-Rice
     * decoder walks the bitstream, so decoding "as many as fit" doesn't yield
     * a short frame, it yields a corrupt one that fails deep inside ag_decompress
     * with nothing pointing back at the real cause (a caller buffer too small).
     * Refuse up front instead. */
    if (out_frames < d->frame_len) return -1;
    uint32_t num_samples = d->frame_len;

    unsigned channel_index = 0;

    while (channel_index < d->channels) {
        if ((uint32_t)(bits.cur - in) >= in_len) return -1;

        uint8_t tag = (uint8_t)bb_read(&bits, 3);
        if (tag == ID_END) break;

        if (tag == ID_SCE || tag == ID_LFE) {
            bb_read(&bits, 4);                 /* element instance tag */
            if (bb_read(&bits, 12) != 0) return -1;   /* unused header, must be 0 */

            uint32_t hdr = bb_read(&bits, 4);
            uint32_t partial = hdr >> 3;
            uint32_t bytes_shifted = (hdr >> 1) & 3;
            uint32_t escape = hdr & 1;
            if (bytes_shifted == 3) return -1;

            uint32_t frame_samples = num_samples;
            if (partial) {
                frame_samples  = bb_read(&bits, 16) << 16;
                frame_samples |= bb_read(&bits, 16);
            }
            uint32_t chan_bits = d->bit_depth - bytes_shifted * 8;

            if (!escape) {
                bb_read(&bits, 8);              /* mixBits, unused for mono */
                bb_read(&bits, 8);              /* mixRes, unused for mono */

                hdr = bb_read(&bits, 8);
                uint32_t mode = hdr >> 4, denshift = hdr & 0xF;
                hdr = bb_read(&bits, 8);
                uint32_t pb_factor = hdr >> 5, numcoef = hdr & 0x1F;

                int16_t coefs[32];
                for (uint32_t i = 0; i < numcoef; i++) coefs[i] = (int16_t)bb_read(&bits, 16);

                if (bytes_shifted) skip_shift_bits(&bits, frame_samples, bytes_shifted);

                ag_params_t ap;
                ag_set(&ap, d->mb, (d->pb * pb_factor) / 4, d->kb);
                if (ag_decompress(&ap, &bits, d->pred, (int32_t)frame_samples, (int32_t)chan_bits) != 0)
                    return -1;

                if (mode == 0) {
                    predictor_reconstruct(d->pred, d->mix_u, (int32_t)frame_samples, coefs, (int32_t)numcoef, chan_bits, denshift);
                } else {
                    predictor_reconstruct(d->pred, d->pred, (int32_t)frame_samples, NULL, 31, chan_bits, 0);
                    predictor_reconstruct(d->pred, d->mix_u, (int32_t)frame_samples, coefs, (int32_t)numcoef, chan_bits, denshift);
                }
            } else {
                uint32_t shift = 32 - chan_bits;
                for (uint32_t i = 0; i < frame_samples; i++) {
                    int32_t v = (int32_t)bb_read(&bits, (uint8_t)chan_bits);
                    d->mix_u[i] = (v << shift) >> shift;
                }
            }

            int net_shift = (int)(bytes_shifted * 8) - (d->bit_depth > 16 ? d->bit_depth - 16 : 0);
            for (uint32_t i = 0; i < frame_samples; i++) {
                int32_t v = d->mix_u[i];
                v = net_shift >= 0 ? (int32_t)((uint32_t)v << net_shift) : (v >> (-net_shift));
                out[(size_t)i * d->channels + channel_index] = (short)v;
            }
            channel_index += 1;

        } else if (tag == ID_CPE) {
            bb_read(&bits, 4);
            if (bb_read(&bits, 12) != 0) return -1;

            uint32_t hdr = bb_read(&bits, 4);
            uint32_t partial = hdr >> 3;
            uint32_t bytes_shifted = (hdr >> 1) & 3;
            uint32_t escape = hdr & 1;
            if (bytes_shifted == 3) return -1;

            uint32_t frame_samples = num_samples;
            if (partial) {
                frame_samples  = bb_read(&bits, 16) << 16;
                frame_samples |= bb_read(&bits, 16);
            }
            uint32_t chan_bits = d->bit_depth - bytes_shifted * 8 + 1;
            int32_t mix_bits = 0, mix_res = 0;

            if (!escape) {
                mix_bits = (int32_t)bb_read(&bits, 8);
                mix_res  = (int8_t)bb_read(&bits, 8);

                hdr = bb_read(&bits, 8);
                uint32_t modeU = hdr >> 4, denshiftU = hdr & 0xF;
                hdr = bb_read(&bits, 8);
                uint32_t pbU = hdr >> 5, numU = hdr & 0x1F;
                int16_t coefsU[32];
                for (uint32_t i = 0; i < numU; i++) coefsU[i] = (int16_t)bb_read(&bits, 16);

                hdr = bb_read(&bits, 8);
                uint32_t modeV = hdr >> 4, denshiftV = hdr & 0xF;
                hdr = bb_read(&bits, 8);
                uint32_t pbV = hdr >> 5, numV = hdr & 0x1F;
                int16_t coefsV[32];
                for (uint32_t i = 0; i < numV; i++) coefsV[i] = (int16_t)bb_read(&bits, 16);

                if (bytes_shifted) skip_shift_bits(&bits, frame_samples * 2, bytes_shifted);

                ag_params_t ap;
                ag_set(&ap, d->mb, (d->pb * pbU) / 4, d->kb);
                if (ag_decompress(&ap, &bits, d->pred, (int32_t)frame_samples, (int32_t)chan_bits) != 0)
                    return -1;
                if (modeU == 0) {
                    predictor_reconstruct(d->pred, d->mix_u, (int32_t)frame_samples, coefsU, (int32_t)numU, chan_bits, denshiftU);
                } else {
                    predictor_reconstruct(d->pred, d->pred, (int32_t)frame_samples, NULL, 31, chan_bits, 0);
                    predictor_reconstruct(d->pred, d->mix_u, (int32_t)frame_samples, coefsU, (int32_t)numU, chan_bits, denshiftU);
                }

                ag_set(&ap, d->mb, (d->pb * pbV) / 4, d->kb);
                if (ag_decompress(&ap, &bits, d->pred, (int32_t)frame_samples, (int32_t)chan_bits) != 0)
                    return -1;
                if (modeV == 0) {
                    predictor_reconstruct(d->pred, d->mix_v, (int32_t)frame_samples, coefsV, (int32_t)numV, chan_bits, denshiftV);
                } else {
                    predictor_reconstruct(d->pred, d->pred, (int32_t)frame_samples, NULL, 31, chan_bits, 0);
                    predictor_reconstruct(d->pred, d->mix_v, (int32_t)frame_samples, coefsV, (int32_t)numV, chan_bits, denshiftV);
                }
            } else {
                chan_bits = d->bit_depth;
                uint32_t shift = 32 - chan_bits;
                for (uint32_t i = 0; i < frame_samples; i++) {
                    int32_t v = (int32_t)bb_read(&bits, (uint8_t)chan_bits);
                    d->mix_u[i] = (v << shift) >> shift;
                    v = (int32_t)bb_read(&bits, (uint8_t)chan_bits);
                    d->mix_v[i] = (v << shift) >> shift;
                }
                mix_bits = mix_res = 0;
            }

            /* mixres/mixbits already bring u/v back to bit_depth-wide L/R
             * (that's the point of the transform), so only the
             * bytesShifted/16-bit-truncation shift is still needed, same
             * as the mono path above -- applied post-unmix in the output
             * loop below rather than to u/v directly. */
            if (bytes_shifted == 0 && d->bit_depth == 16) {
                unmix_s16(d->mix_u, d->mix_v, out + channel_index, (int)d->channels,
                         (int32_t)frame_samples, mix_bits, mix_res);
            } else {
                int net_shift = (int)(bytes_shifted * 8) - (d->bit_depth > 16 ? d->bit_depth - 16 : 0);
                for (uint32_t i = 0; i < frame_samples; i++) {
                    int32_t l, r;
                    if (mix_res != 0) {
                        l = d->mix_u[i] + d->mix_v[i] - ((mix_res * d->mix_v[i]) >> mix_bits);
                        r = l - d->mix_v[i];
                    } else {
                        l = d->mix_u[i]; r = d->mix_v[i];
                    }
                    l = net_shift >= 0 ? (int32_t)((uint32_t)l << net_shift) : (l >> (-net_shift));
                    r = net_shift >= 0 ? (int32_t)((uint32_t)r << net_shift) : (r >> (-net_shift));
                    out[(size_t)i * d->channels + channel_index]     = (short)l;
                    out[(size_t)i * d->channels + channel_index + 1] = (short)r;
                }
            }
            channel_index += 2;

        } else if (tag == ID_DSE || tag == ID_FIL) {
            uint32_t count;
            if (tag == ID_DSE) {
                bb_read(&bits, 4);              /* element instance tag */
                uint32_t align = bb_read1(&bits);
                count = bb_read(&bits, 8);
                if (count == 255) count += bb_read(&bits, 8);
                if (align) bb_byte_align(&bits);
            } else {
                count = bb_read(&bits, 4);
                if (count == 15) count += bb_read(&bits, 8) - 1;
            }
            bb_advance(&bits, count * 8);
        } else {
            /* CCE/PCE or a corrupt tag: nothing sane to skip. */
            return -1;
        }
    }

    if (channel_index < d->channels) return -1;   /* fewer channels than configured */
    return (int)num_samples;
}
