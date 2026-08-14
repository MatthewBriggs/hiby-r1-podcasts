/* ogg_io.h — minimal Ogg page/packet demuxer, shared by Vorbis and Opus.
 *
 * Wraps libogg's ogg_sync_state/ogg_stream_state (page sync + packet
 * reassembly) around a FILE*. Codec-agnostic: it just hands back whichever
 * logical stream's packets follow the file's first page, which is all a
 * single-audio-track file (the only kind either decoder here deals with)
 * ever needs — a chained/multiplexed Ogg file is out of scope.
 */
#ifndef OGG_IO_H
#define OGG_IO_H

#include <stdio.h>
#include "vendor/ogg/ogg.h"

typedef struct {
    FILE *fp;
    ogg_sync_state oy;
    ogg_stream_state os;
    int stream_ready;
    int eof;                /* underlying file hit EOF while filling oy */
    long file_size;
} ogg_io_t;

/* Opens the file and reads/pages-in up to the first packet of the first
 * logical stream, without consuming it — the caller's first
 * ogg_io_next_packet() call returns that same packet, so a header parser can
 * always start from packet 0 regardless of how ogg_io_open() found it. */
int  ogg_io_open(ogg_io_t *g, const char *path);

/* Fills `op` with the next packet of the stream `ogg_io_open` selected.
 * Returns 1 on a packet, 0 at end of stream, -1 on error. `op->packet` points
 * into libogg's own internal buffer and is only valid until the next call. */
int  ogg_io_next_packet(ogg_io_t *g, ogg_packet *op);

/* Seeks so the next ogg_io_next_packet() returns a packet from the page
 * whose granulepos is the closest one at or before `target_granule` that
 * bisection search can find — the standard approach (libvorbisfile's, in
 * essence): binary-search file offsets, resync to the next page boundary
 * from each guess, and use that page's own granulepos to narrow the range.
 * Exact only up to "which page"; the caller decodes forward from the page
 * start to the exact target sample, same as every other seek path in this
 * app already does with a coarse container-level seek. Returns 0 on success. */
int  ogg_io_seek(ogg_io_t *g, int64_t target_granule);

void ogg_io_close(ogg_io_t *g);

#endif
