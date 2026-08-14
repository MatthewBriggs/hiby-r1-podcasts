/* hls.c — enough HLS to play a live radio stream.
 *
 * Two playlists: a master listing variants at different bitrates, and a media
 * playlist per variant listing the segments currently available. The media
 * playlist is a sliding window — it is re-fetched as it goes, and segments
 * already played are recognised by their media sequence number rather than by
 * their URL, because a stream can legitimately repeat a filename.
 *
 * Fetching goes through the static curl on the card, as elsewhere in this app:
 * the device's own wget cannot complete a modern TLS handshake.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hls.h"

#define CURL_PATH "/data/mnt/sd_0/.podsync/curl"
#define CA_BUNDLE "/data/mnt/sd_0/.podsync/cacert.pem"

static int fetch(const char *url, unsigned char *buf, int max) {
    char cmd[HLS_URL_MAX + 256];
    snprintf(cmd, sizeof(cmd),
             "%s -sL --max-time 20 --cacert %s '%s' 2>/dev/null",
             CURL_PATH, CA_BUNDLE, url);
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    int n = 0;
    while (n < max) {
        size_t got = fread(buf + n, 1, (size_t)(max - n), p);
        if (got == 0) break;
        n += (int)got;
    }
    pclose(p);
    return n;
}

/* Resolve a playlist entry against the playlist it came from: absolute, or
 * host-relative, or a sibling. */
static void resolve(const char *base, const char *ref, char *out, size_t n) {
    if (!strncmp(ref, "http://", 7) || !strncmp(ref, "https://", 8)) {
        snprintf(out, n, "%s", ref);
        return;
    }
    if (ref[0] == '/') {
        const char *p = strstr(base, "://");
        p = p ? strchr(p + 3, '/') : NULL;
        int host_len = p ? (int)(p - base) : (int)strlen(base);
        snprintf(out, n, "%.*s%s", host_len, base, ref);
        return;
    }
    const char *slash = strrchr(base, '/');
    int dir = slash ? (int)(slash - base) : (int)strlen(base);
    snprintf(out, n, "%.*s/%s", dir, base, ref);
}

static char *next_line(char **cursor) {
    char *s = *cursor;
    if (!s || !*s) return NULL;
    char *nl = strchr(s, '\n');
    if (nl) { *nl = '\0'; *cursor = nl + 1; } else { *cursor = s + strlen(s); }
    size_t len = strlen(s);
    while (len && (s[len - 1] == '\r' || s[len - 1] == ' ')) s[--len] = '\0';
    return s;
}

int hls_open(hls_t *h, const char *master_url) {
    memset(h, 0, sizeof(*h));
    h->last_seq = -1;
    h->target_ms = 4000;

    static unsigned char buf[256 * 1024];
    int n = fetch(master_url, buf, (int)sizeof(buf) - 1);
    if (n <= 0) return -1;
    buf[n] = '\0';
    if (strncmp((char *)buf, "#EXTM3U", 7) != 0) return -1;

    /* A media playlist can be handed to us directly; only chase variants when
     * this is actually a master. */
    if (!strstr((char *)buf, "#EXT-X-STREAM-INF")) {
        snprintf(h->media_url, sizeof(h->media_url), "%s", master_url);
        return 0;
    }

    /* Pick the highest bandwidth on offer. This is a music player attached to
     * a DAC; the point is the good one. */
    long best = -1;
    char *cur = (char *)buf, *line;
    long pending_bw = -1;
    while ((line = next_line(&cur))) {
        if (!strncmp(line, "#EXT-X-STREAM-INF", 17)) {
            const char *b = strstr(line, "BANDWIDTH=");
            pending_bw = b ? strtol(b + 10, NULL, 10) : 0;
        } else if (line[0] && line[0] != '#' && pending_bw >= 0) {
            if (pending_bw > best) {
                best = pending_bw;
                resolve(master_url, line, h->media_url, sizeof(h->media_url));
            }
            pending_bw = -1;
        }
    }
    return h->media_url[0] ? 0 : -1;
}

/* Re-read the media playlist and queue anything newer than what we have. */
static int refresh(hls_t *h) {
    static unsigned char buf[256 * 1024];
    int n = fetch(h->media_url, buf, (int)sizeof(buf) - 1);
    if (n <= 0) return -1;
    buf[n] = '\0';

    long long seq = 0;
    char *cur = (char *)buf, *line;
    while ((line = next_line(&cur))) {
        if (!strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22)) {
            seq = strtoll(line + 22, NULL, 10);
        } else if (!strncmp(line, "#EXT-X-TARGETDURATION:", 22)) {
            int t = (int)strtol(line + 22, NULL, 10);
            if (t > 0 && t < 60) h->target_ms = t * 1000;
        } else if (line[0] && line[0] != '#') {
            /* On the first pass start near the live edge rather than at the
             * back of the window, which would begin playback a minute late. */
            if (h->last_seq < 0 || seq > h->last_seq) {
                if (h->n_queue < HLS_QUEUE) {
                    resolve(h->media_url, line, h->queue[h->n_queue], HLS_URL_MAX);
                    h->n_queue++;
                    h->last_seq = seq;
                }
            }
            seq++;
        }
    }
    /* Starting cold, drop all but the last couple so playback begins live. */
    if (h->n_queue > 2) {
        memmove(h->queue[0], h->queue[h->n_queue - 2], HLS_URL_MAX);
        memmove(h->queue[1], h->queue[h->n_queue - 1], HLS_URL_MAX);
        h->n_queue = 2;
    }
    return h->n_queue;
}

int hls_next(hls_t *h, unsigned char *buf, int max) {
    if (h->n_queue == 0 && refresh(h) <= 0) return 0;
    if (h->n_queue == 0) return 0;

    int n = fetch(h->queue[0], buf, max);
    memmove(h->queue[0], h->queue[1], (size_t)(HLS_QUEUE - 1) * HLS_URL_MAX);
    h->n_queue--;
    return n > 0 ? n : 0;
}
