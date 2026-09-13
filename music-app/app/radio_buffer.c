/* radio_buffer.c -- see radio_buffer.h for the design.
 *
 * Chunk files are named by (sequence number mod RB_MAX_CHUNKS), so the
 * retention window falls out of the naming scheme itself: writing chunk
 * seq N+RB_MAX_CHUNKS overwrites (O_TRUNC) the same file that held chunk N,
 * which is exactly the eviction policy wanted -- no separate delete pass,
 * no manifest compaction, just "the oldest chunk still fitting in the ring
 * of filenames".
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "radio_buffer.h"
#include "hls.h"
#include "ts.h"

#define RB_DIR            "/data/mnt/sd_0/.radio_buffer"
#define RB_CHUNK_SECONDS  20
#define RB_WINDOW_SECONDS (30 * 60)
#define RB_MAX_CHUNKS     ((RB_WINDOW_SECONDS / RB_CHUNK_SECONDS) + 2)  /* +2 slack */

#define RB_CURL_PATH "/data/mnt/sd_0/.podsync/curl"
#define RB_CA_BUNDLE "/data/mnt/sd_0/.podsync/cacert.pem"

typedef struct {
    long long seq;       /* -1 = slot never written this session */
    uint64_t  start_pos;
    size_t    len;
} rb_chunk_meta_t;

static pthread_t       g_thread;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int    g_running;   /* background thread told to keep going */
static volatile int    g_active;    /* background thread actually alive */
static rb_kind_t       g_kind;
static char            g_url[1024];

static rb_chunk_meta_t g_chunks[RB_MAX_CHUNKS];
static uint64_t        g_live_pos;      /* total bytes ever written */
static long long       g_cur_seq = -1;  /* chunk currently being written */

/* Bytes/sec estimate: exponential moving average over ~4s windows of real
 * arrival, not a nominal bitrate the source is not obliged to honour. */
static double   g_bps;
static uint64_t g_bps_window_bytes;

static void chunk_path(long long seq, char *out, size_t n) {
    snprintf(out, n, "%s/%lld.chunk", RB_DIR, seq % RB_MAX_CHUNKS);
}

static double now_mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void rb_note_bytes(size_t n) {
    static double window_start = -1;
    double t = now_mono();
    if (window_start < 0) { window_start = t; g_bps_window_bytes = 0; }
    g_bps_window_bytes += n;
    double elapsed = t - window_start;
    if (elapsed >= 4.0) {
        double measured = (double)g_bps_window_bytes / elapsed;
        g_bps = (g_bps <= 0) ? measured : (g_bps * 0.5 + measured * 0.5);
        window_start = t;
        g_bps_window_bytes = 0;
    }
}

/* Start (or roll to) the chunk for the given global write position. Slot
 * (seq mod RB_MAX_CHUNKS) is reused unconditionally -- whatever chunk lived
 * there before is exactly RB_MAX_CHUNKS chunks old, which is the retention
 * limit, so overwriting it is the eviction. */
static int rb_roll_chunk(long long seq) {
    char path[300];
    chunk_path(seq, path, sizeof(path));
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) return -1;
    pthread_mutex_lock(&g_lock);
    g_chunks[seq % RB_MAX_CHUNKS].seq = seq;
    g_chunks[seq % RB_MAX_CHUNKS].start_pos = g_live_pos;
    g_chunks[seq % RB_MAX_CHUNKS].len = 0;
    g_cur_seq = seq;
    pthread_mutex_unlock(&g_lock);
    return fd;
}

static void rb_write_bytes(int *fd, double *chunk_started, const unsigned char *data, size_t n) {
    if (n == 0) return;
    ssize_t w = write(*fd, data, n);
    if (w < 0) w = 0;
    pthread_mutex_lock(&g_lock);
    g_chunks[g_cur_seq % RB_MAX_CHUNKS].len += (size_t)w;
    g_live_pos += (uint64_t)w;
    pthread_mutex_unlock(&g_lock);
    rb_note_bytes((size_t)w);

    if (now_mono() - *chunk_started >= RB_CHUNK_SECONDS) {
        close(*fd);
        *fd = rb_roll_chunk(g_cur_seq + 1);
        *chunk_started = now_mono();
    }
}

static void *rb_worker_mp3(void *arg) {
    (void)arg;
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "%s -sL --no-buffer --cacert %s '%s' 2>/dev/null",
             RB_CURL_PATH, RB_CA_BUNDLE, g_url);
    FILE *p = popen(cmd, "r");
    if (!p) { g_active = 0; return NULL; }

    int fd = rb_roll_chunk(0);
    double chunk_started = now_mono();
    if (fd < 0) { pclose(p); g_active = 0; return NULL; }

    unsigned char buf[8192];
    while (g_running) {
        size_t got = fread(buf, 1, sizeof(buf), p);
        if (got == 0) {
            if (feof(p) || ferror(p)) break;
            continue;
        }
        rb_write_bytes(&fd, &chunk_started, buf, got);
    }
    close(fd);
    pclose(p);
    g_active = 0;
    return NULL;
}

static void *rb_worker_hls(void *arg) {
    (void)arg;
    hls_t hls;
    if (hls_open(&hls, g_url) != 0) { g_active = 0; return NULL; }

    int fd = rb_roll_chunk(0);
    double chunk_started = now_mono();
    if (fd < 0) { g_active = 0; return NULL; }

    static unsigned char seg[512 * 1024];
    static unsigned char adts[192 * 1024];
    int stalls = 0;
    while (g_running) {
        int n = hls_next(&hls, seg, (int)sizeof(seg));
        if (n <= 0) {
            if (++stalls > 30) break;   /* ~ a minute of nothing new at 4s segments */
            usleep(500 * 1000);
            continue;
        }
        stalls = 0;
        int a = ts_extract_audio(seg, n, adts, (int)sizeof(adts));
        if (a > 0) rb_write_bytes(&fd, &chunk_started, adts, (size_t)a);
    }
    close(fd);
    g_active = 0;
    return NULL;
}

int rb_start(const char *url, rb_kind_t kind) {
    rb_stop();

    mkdir(RB_DIR, 0755);
    /* Wipe the previous session's chunk files -- a different station starts
     * from a clean, empty window, not a tail of whatever the last one left
     * behind under the same (seq mod RB_MAX_CHUNKS) filenames. */
    for (int i = 0; i < RB_MAX_CHUNKS; i++) {
        char path[300];
        snprintf(path, sizeof(path), "%s/%d.chunk", RB_DIR, i);
        unlink(path);
        g_chunks[i].seq = -1;
        g_chunks[i].len = 0;
    }
    g_live_pos = 0;
    g_cur_seq = -1;
    g_bps = 0;
    g_bps_window_bytes = 0;

    snprintf(g_url, sizeof(g_url), "%s", url);
    g_kind = kind;
    g_running = 1;
    g_active = 1;
    void *(*fn)(void *) = (kind == RB_KIND_ADTS) ? rb_worker_hls : rb_worker_mp3;
    if (pthread_create(&g_thread, NULL, fn, NULL) != 0) {
        g_running = 0;
        g_active = 0;
        return -1;
    }
    return 0;
}

void rb_stop(void) {
    if (!g_running && !g_active) return;
    g_running = 0;
    pthread_join(g_thread, NULL);
    g_active = 0;
}

uint64_t rb_live_pos(void) {
    pthread_mutex_lock(&g_lock);
    uint64_t v = g_live_pos;
    pthread_mutex_unlock(&g_lock);
    return v;
}

uint64_t rb_oldest_pos(void) {
    pthread_mutex_lock(&g_lock);
    uint64_t oldest = g_live_pos;
    for (int i = 0; i < RB_MAX_CHUNKS; i++) {
        if (g_chunks[i].seq < 0) continue;
        if (g_chunks[i].start_pos < oldest) oldest = g_chunks[i].start_pos;
    }
    pthread_mutex_unlock(&g_lock);
    return oldest;
}

size_t rb_read_at(uint64_t pos, unsigned char *out, size_t len) {
    if (pos >= rb_live_pos()) return 0;
    if (pos < rb_oldest_pos()) return 0;

    pthread_mutex_lock(&g_lock);
    rb_chunk_meta_t chunks[RB_MAX_CHUNKS];
    memcpy(chunks, g_chunks, sizeof(chunks));
    pthread_mutex_unlock(&g_lock);

    /* Find the chunk containing pos. */
    for (int i = 0; i < RB_MAX_CHUNKS; i++) {
        if (chunks[i].seq < 0) continue;
        uint64_t lo = chunks[i].start_pos, hi = lo + chunks[i].len;
        if (pos < lo || pos >= hi) continue;
        char path[300];
        chunk_path(chunks[i].seq, path, sizeof(path));
        int fd = open(path, O_RDONLY);
        if (fd < 0) return 0;
        lseek(fd, (off_t)(pos - lo), SEEK_SET);
        size_t want = len;
        if (want > hi - pos) want = (size_t)(hi - pos);
        ssize_t got = read(fd, out, want);
        close(fd);
        if (got <= 0) return 0;
        size_t total = (size_t)got;
        /* A request spanning into the next chunk: recurse once. Chunks are
         * contiguous in position by construction (each starts exactly where
         * the previous one's live_pos was), so pos+total is the very next
         * byte position with no gap to bridge. */
        if (total < len && pos + total < rb_live_pos())
            total += rb_read_at(pos + total, out + total, len - total);
        return total;
    }
    return 0;
}

double rb_bytes_per_sec(void) {
    pthread_mutex_lock(&g_lock);
    double v = g_bps;
    pthread_mutex_unlock(&g_lock);
    return v;
}

int rb_active(void) { return g_active; }
rb_kind_t rb_current_kind(void) { return g_kind; }
