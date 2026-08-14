/* ts.c — the audio out of an MPEG-TS segment.
 *
 * A transport stream is 188-byte packets, each with a 4-byte header carrying a
 * 13-bit PID. Two of those PIDs are directories: PID 0 is the PAT, which names
 * the PID of the PMT, which in turn lists the streams and their types. Audio
 * is type 0x0F (AAC in ADTS) or 0x11 (LATM); only ADTS is handled, because
 * that is what every HLS radio stream here uses and because the decoder can
 * then parse the framing itself.
 *
 * The payload is PES-packetised: a 6-byte header, then a variable-length
 * extension whose size is given in the ninth byte. Strip those and what is
 * left is the ADTS stream.
 *
 * Deliberately stateless — each HLS segment repeats its PAT and PMT, so there
 * is nothing to carry between them, and a dropped segment cannot desync it.
 */

#include <string.h>

#include "ts.h"

#define TS_PKT 188

static int pid_of(const unsigned char *p) {
    return ((p[1] & 0x1F) << 8) | p[2];
}

/* Payload start, accounting for the adaptation field. Returns -1 if this
 * packet has no payload at all. */
static int payload_at(const unsigned char *p) {
    int afc = (p[3] >> 4) & 3;
    if (afc == 0 || afc == 2) return -1;        /* no payload */
    if (afc == 1) return 4;
    int alen = p[4];
    int off = 5 + alen;
    return off < TS_PKT ? off : -1;
}

int ts_extract_audio(const unsigned char *seg, int len,
                     unsigned char *out, int out_max) {
    int pmt_pid = -1, audio_pid = -1, n = 0;

    /* Pass one: find the audio PID via PAT then PMT. */
    for (int i = 0; i + TS_PKT <= len; i += TS_PKT) {
        const unsigned char *p = seg + i;
        if (p[0] != 0x47) return -1;
        int pid = pid_of(p);
        int off = payload_at(p);
        if (off < 0) continue;
        int pusi = p[1] & 0x40;
        /* A section payload begins with a pointer_field to skip. */
        if (pusi) off += 1 + p[off];
        if (off >= TS_PKT) continue;

        if (pid == 0 && pmt_pid < 0) {
            const unsigned char *s = p + off;
            int slen = ((s[1] & 0x0F) << 8) | s[2];
            /* 8 bytes of section header, then 4-byte program entries, less the
             * trailing CRC. */
            for (int k = 8; k + 4 <= slen - 1; k += 4) {
                int prog = (s[k] << 8) | s[k + 1];
                int p_pid = ((s[k + 2] & 0x1F) << 8) | s[k + 3];
                if (prog != 0) { pmt_pid = p_pid; break; }
            }
        } else if (pid == pmt_pid && audio_pid < 0) {
            const unsigned char *s = p + off;
            int slen = ((s[1] & 0x0F) << 8) | s[2];
            int pil = ((s[10] & 0x0F) << 8) | s[11];
            for (int k = 12 + pil; k + 5 <= slen - 1; ) {
                int type = s[k];
                int e_pid = ((s[k + 1] & 0x1F) << 8) | s[k + 2];
                int esl = ((s[k + 3] & 0x0F) << 8) | s[k + 4];
                if (type == 0x0F) { audio_pid = e_pid; break; }
                k += 5 + esl;
            }
        }
    }
    if (audio_pid < 0) return -1;

    /* Pass two: strip TS and PES framing off that PID. */
    for (int i = 0; i + TS_PKT <= len; i += TS_PKT) {
        const unsigned char *p = seg + i;
        if (pid_of(p) != audio_pid) continue;
        int off = payload_at(p);
        if (off < 0) continue;
        if (p[1] & 0x40) {
            const unsigned char *pes = p + off;
            int avail = TS_PKT - off;
            if (avail >= 9 && pes[0] == 0 && pes[1] == 0 && pes[2] == 1)
                off += 9 + pes[8];              /* PES header + extension */
        }
        if (off >= TS_PKT) continue;
        int take = TS_PKT - off;
        if (n + take > out_max) break;
        memcpy(out + n, p + off, (size_t)take);
        n += take;
    }
    return n;
}
