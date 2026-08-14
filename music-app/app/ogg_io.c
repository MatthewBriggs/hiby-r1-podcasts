/* ogg_io.c — see ogg_io.h. */
#include <stdlib.h>
#include <string.h>
#include "ogg_io.h"

#define READ_CHUNK 8192

static int fill_more(ogg_io_t *g) {
    if (g->eof) return 0;
    char *buf = ogg_sync_buffer(&g->oy, READ_CHUNK);
    if (!buf) return 0;
    size_t n = fread(buf, 1, READ_CHUNK, g->fp);
    ogg_sync_wrote(&g->oy, (long)n);
    if (n == 0) { g->eof = 1; return 0; }
    return 1;
}

int ogg_io_open(ogg_io_t *g, const char *path) {
    memset(g, 0, sizeof(*g));
    g->fp = fopen(path, "rb");
    if (!g->fp) return -1;
    fseek(g->fp, 0, SEEK_END);
    g->file_size = ftell(g->fp);
    fseek(g->fp, 0, SEEK_SET);

    ogg_sync_init(&g->oy);

    ogg_page page;
    int got = 0;
    while (!got) {
        int rc = ogg_sync_pageout(&g->oy, &page);
        if (rc == 1) { got = 1; break; }
        if (!fill_more(g)) break;
    }
    if (!got) { ogg_io_close(g); return -1; }

    ogg_stream_init(&g->os, ogg_page_serialno(&page));
    ogg_stream_pagein(&g->os, &page);
    g->stream_ready = 1;
    return 0;
}

int ogg_io_next_packet(ogg_io_t *g, ogg_packet *op) {
    if (!g->stream_ready) return -1;
    for (;;) {
        int rc = ogg_stream_packetout(&g->os, op);
        if (rc == 1) return 1;
        /* rc == 0: needs another page. rc == -1: a gap in the data (a
         * corrupt or truncated file) -- not fatal here, just keep pulling
         * pages and let the stream resync on its own, same as any streaming
         * player has to tolerate. */
        ogg_page page;
        int got_page = 0;
        for (;;) {
            int prc = ogg_sync_pageout(&g->oy, &page);
            if (prc == 1) { got_page = 1; break; }
            if (!fill_more(g)) break;
        }
        if (!got_page) return 0;   /* end of stream */
        if (ogg_page_serialno(&page) != g->os.serialno) continue;
        ogg_stream_pagein(&g->os, &page);
    }
}

int ogg_io_seek(ogg_io_t *g, int64_t target_granule) {
    if (!g->fp || g->file_size <= 0) return -1;
    long serialno = g->os.serialno;
    long lo = 0, hi = g->file_size;
    long best_pos = -1;
    int64_t best_granule = -1;

    for (int iter = 0; iter < 40 && hi - lo > 4096; iter++) {
        long mid = lo + (hi - lo) / 2;
        fseek(g->fp, mid, SEEK_SET);
        ogg_sync_reset(&g->oy);
        g->eof = 0;

        long consumed = 0;
        long found_pos = -1;
        int64_t found_granule = -1;
        ogg_page page;
        /* Scan forward from `mid` for the first complete page belonging to
         * our stream with a real (non -1) granulepos -- a page mid-packet
         * carries no usable position, and the next one that does is a fine
         * substitute since decoding resumes from a page boundary either way. */
        for (int guard = 0; guard < 4096; guard++) {
            long rc = ogg_sync_pageseek(&g->oy, &page);
            if (rc > 0) {
                long page_start = mid + consumed;
                consumed += rc;
                if (ogg_page_serialno(&page) == serialno) {
                    int64_t gp = ogg_page_granulepos(&page);
                    if (gp >= 0) { found_pos = page_start; found_granule = gp; break; }
                }
                continue;
            }
            if (rc < 0) { consumed += -rc; continue; }
            /* rc == 0: not enough data buffered yet. */
            if (!fill_more(g)) break;
        }
        if (found_pos < 0) { hi = mid; continue; }   /* nothing usable in the tail half */

        if (found_granule < target_granule) {
            lo = mid;
            best_pos = found_pos;
            best_granule = found_granule;
        } else {
            hi = mid;
        }
    }

    if (best_pos < 0) { best_pos = 0; }
    (void)best_granule;

    fseek(g->fp, best_pos, SEEK_SET);
    ogg_sync_reset(&g->oy);
    g->eof = 0;
    ogg_stream_reset_serialno(&g->os, (int)serialno);

    /* Feed pages forward from best_pos until the stream has one pageined --
     * ogg_io_next_packet takes over from there exactly as it does after
     * ogg_io_open(). */
    ogg_page page;
    for (;;) {
        int rc = ogg_sync_pageout(&g->oy, &page);
        if (rc == 1) {
            if (ogg_page_serialno(&page) != serialno) continue;
            ogg_stream_pagein(&g->os, &page);
            return 0;
        }
        if (!fill_more(g)) return -1;
    }
}

void ogg_io_close(ogg_io_t *g) {
    if (g->stream_ready) ogg_stream_clear(&g->os);
    ogg_sync_clear(&g->oy);
    if (g->fp) fclose(g->fp);
    memset(g, 0, sizeof(*g));
}
