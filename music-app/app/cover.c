/* cover.c — feed cover art: JPEG decode to RGB565, cached on the card.
 *
 * The device ships libjpeg 9 (/usr/lib/libjpeg.so.9) but no headers, and it is
 * dlopen'd rather than linked so a missing or mismatched library degrades to
 * "no cover" instead of failing to load the app. That means the struct layout
 * has to be described here, which is why libjpeg 9's own headers are vendored:
 * jpeg_CreateDecompress validates sizeof(struct jpeg_decompress_struct) and
 * refuses to run if it disagrees, so a wrong header fails safely but silently.
 *
 * Decoding a 3000x3000 cover on a 56 MB device is not free, so libjpeg's
 * scale_denom does the downscale during decode (it can skip most of the IDCT
 * work), and the result is cached next to the cover as a raw RGB565 blob.
 *
 * Cover art is untrusted input — whatever the podcast host chose to publish —
 * and a 3000x3000 *progressive* cover was enough to get hiby_player killed by
 * the OOM killer. Scanline streaming below keeps baseline JPEGs cheap at any
 * size, but progressive decoding cannot stream: jpeg_start_decompress builds
 * the whole coefficient array before it will yield a single line, and
 * scale_denom shrinks only the output, not that array. At 3000x3000x3 that is
 * ~54 MB on a device with about 18 MB free. So the header is checked first and
 * anything whose working set would not fit is declined; the feed loses its
 * thumbnail instead of the player losing its life.
 */

#include <dlfcn.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "vendor/jpeg9/jpeglib.h"
#include "cover.h"

typedef void (*pfn_create)(j_decompress_ptr, int, size_t);
typedef struct jpeg_error_mgr *(*pfn_std_error)(struct jpeg_error_mgr *);

static void *g_lib;
static pfn_create    x_create;
static pfn_std_error x_std_error;
static void (*x_stdio_src)(j_decompress_ptr, FILE *);
static int  (*x_read_header)(j_decompress_ptr, boolean);
static boolean (*x_start)(j_decompress_ptr);
static JDIMENSION (*x_read_scanlines)(j_decompress_ptr, JSAMPARRAY, JDIMENSION);
static boolean (*x_finish)(j_decompress_ptr);
static void (*x_destroy)(j_decompress_ptr);
static void (*x_calc_dims)(j_decompress_ptr);

/* Roughly half the free memory on an idle device, so a decode cannot crowd out
 * the player even at the worst moment. Anything larger loses its thumbnail. */
#define COVER_MEM_BUDGET  (8 * 1024 * 1024)
#define COVER_MAX_DIM     8000

static int g_tried;

static int load_lib(void) {
    if (g_tried) return g_lib != NULL;
    g_tried = 1;
    g_lib = dlopen("libjpeg.so.9", RTLD_NOW);
    if (!g_lib) g_lib = dlopen("/usr/lib/libjpeg.so.9", RTLD_NOW);
    if (!g_lib) g_lib = dlopen("libjpeg.so", RTLD_NOW);
    if (!g_lib) return 0;

    x_create        = (pfn_create)dlsym(g_lib, "jpeg_CreateDecompress");
    x_std_error     = (pfn_std_error)dlsym(g_lib, "jpeg_std_error");
    x_stdio_src     = (void (*)(j_decompress_ptr, FILE *))dlsym(g_lib, "jpeg_stdio_src");
    x_read_header   = (int (*)(j_decompress_ptr, boolean))dlsym(g_lib, "jpeg_read_header");
    x_start         = (boolean (*)(j_decompress_ptr))dlsym(g_lib, "jpeg_start_decompress");
    x_read_scanlines= (JDIMENSION (*)(j_decompress_ptr, JSAMPARRAY, JDIMENSION))dlsym(g_lib, "jpeg_read_scanlines");
    x_finish        = (boolean (*)(j_decompress_ptr))dlsym(g_lib, "jpeg_finish_decompress");
    x_destroy       = (void (*)(j_decompress_ptr))dlsym(g_lib, "jpeg_destroy_decompress");
    x_calc_dims     = (void (*)(j_decompress_ptr))dlsym(g_lib, "jpeg_calc_output_dimensions");

    if (!x_create || !x_std_error || !x_stdio_src || !x_read_header ||
        !x_start || !x_read_scanlines || !x_finish || !x_destroy) {
        dlclose(g_lib);
        g_lib = NULL;
        return 0;
    }
    return 1;
}

/* libjpeg's default error handler calls exit(); jump out instead. */
struct jump_err {
    struct jpeg_error_mgr pub;
    jmp_buf jump;
};

static void on_error(j_common_ptr cinfo) {
    struct jump_err *e = (struct jump_err *)cinfo->err;
    longjmp(e->jump, 1);
}

static void on_message(j_common_ptr cinfo) { (void)cinfo; }   /* stay quiet */

/* The podcast app caches the scaled bitmap beside the JPEG it came from. That
 * will not do here: the JPEGs are in the user's own album folders, and leaving
 * a .r565 in every one of them is vandalism.
 *
 * Nor can the cache live on /usr/data, which is a 36 MB partition with about
 * 27 MB free. One bitmap is a third of a megabyte and this library has 293
 * albums; caching them internally would fill the device. It goes on the card,
 * which is where the music is — if the card is out there is nothing to play
 * anyway. */
#define COVER_CACHE_DIR "/data/mnt/sd_0/.music_covers"

static void cache_path(const char *key, int px, char *out, size_t n) {
    unsigned long h = 5381;
    for (const unsigned char *p = (const unsigned char *)key; *p; p++)
        h = ((h << 5) + h) ^ *p;
    mkdir(COVER_CACHE_DIR, 0755);
    snprintf(out, n, "%s/%08lx.%d.r565", COVER_CACHE_DIR, h & 0xFFFFFFFFul, px);
}

static uint16_t *load_cache(const char *jpg, const char *key, int px) {
    char p[512];
    cache_path(key, px, p, sizeof(p));

    /* Replacing cover.jpg by hand is the documented way to give a feed art the
     * decoder refuses (see the progressive-JPEG limit), so a cache older than
     * the file it came from has to lose. */
    /* Only meaningful for artwork that lives somewhere permanent. Art
     * extracted from inside a music file is decoded from a scratch file in
     * /tmp that is rewritten every time, and comparing against that would
     * throw the cache away on every single play. */
    struct stat cs, js;
    if (strncmp(jpg, "/tmp/", 5) != 0 &&
        stat(p, &cs) == 0 && stat(jpg, &js) == 0 && js.st_mtime > cs.st_mtime) {
        unlink(p);
        return NULL;
    }

    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    size_t want = (size_t)px * px;
    uint16_t *buf = malloc(want * sizeof(uint16_t));
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, sizeof(uint16_t), want, f);
    fclose(f);
    if (got != want) { free(buf); return NULL; }
    return buf;
}

/* The cache is on the card, so it is not a threat to the device, but it still
 * grows by a third of a megabyte per album and nothing was ever removing any
 * of it. Keep a bounded number of the most recently used and drop the rest. */
#define COVER_CACHE_KEEP 120

static void prune_cache(void) {
    DIR *d = opendir(COVER_CACHE_DIR);
    if (!d) return;
    struct { char name[64]; time_t at; } ent[512];
    int n = 0;
    struct dirent *e;
    while (n < (int)(sizeof(ent) / sizeof(ent[0])) && (e = readdir(d))) {
        if (!strstr(e->d_name, ".r565")) continue;
        char p[512];
        snprintf(p, sizeof(p), "%s/%s", COVER_CACHE_DIR, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        snprintf(ent[n].name, sizeof(ent[n].name), "%s", e->d_name);
        ent[n].at = st.st_mtime;
        n++;
    }
    closedir(d);
    if (n <= COVER_CACHE_KEEP) return;

    /* Selection sort by age; n is small and this runs only when the cache is
     * already over its limit. */
    for (int i = 0; i < n - COVER_CACHE_KEEP; i++) {
        int oldest = i;
        for (int j = i + 1; j < n; j++)
            if (ent[j].at < ent[oldest].at) oldest = j;
        if (oldest != i) {
            typeof(ent[0]) t = ent[i]; ent[i] = ent[oldest]; ent[oldest] = t;
        }
        char p[512];
        snprintf(p, sizeof(p), "%s/%s", COVER_CACHE_DIR, ent[i].name);
        unlink(p);
    }
}

static void save_cache(const char *key, int px, const uint16_t *buf) {
    char p[512];
    cache_path(key, px, p, sizeof(p));
    FILE *f = fopen(p, "wb");
    if (!f) return;
    fwrite(buf, sizeof(uint16_t), (size_t)px * px, f);
    fclose(f);
    prune_cache();
}

uint16_t *cover_cached(const char *cache_key, const char *dir, int px) {
    if (px <= 0 || px > 512 || !cache_key) return NULL;
    char p[512];
    cache_path(cache_key, px, p, sizeof(p));

    /* Same staleness rule load_cache() applies to a JPEG, against the folder
     * instead: adding or replacing artwork in an album directory bumps the
     * directory's own mtime, so a cache entry older than the folder is thrown
     * away rather than pinning yesterday's cover forever. Replacing a file
     * fully in place on exFAT may not move the folder's mtime -- that case
     * still resolves on the next cache prune, or by deleting the entry. */
    struct stat cs, ds;
    if (stat(p, &cs) != 0) return NULL;
    if (dir && stat(dir, &ds) == 0 && ds.st_mtime > cs.st_mtime) {
        unlink(p);
        return NULL;
    }

    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    size_t want = (size_t)px * px;
    uint16_t *buf = malloc(want * sizeof(uint16_t));
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, sizeof(uint16_t), want, f);
    fclose(f);
    if (got != want) { free(buf); return NULL; }
    return buf;
}

uint16_t *cover_load(const char *jpeg_path, const char *cache_key, int px) {
    if (px <= 0 || px > 512) return NULL;
    if (!cache_key) cache_key = jpeg_path;

    uint16_t *cached = load_cache(jpeg_path, cache_key, px);
    if (cached) return cached;
    if (!load_lib()) return NULL;

    FILE *f = fopen(jpeg_path, "rb");
    if (!f) return NULL;

    struct jpeg_decompress_struct cinfo;
    struct jump_err jerr;
    uint16_t *out = NULL;
    JSAMPLE *row = NULL;

    memset(&cinfo, 0, sizeof(cinfo));
    cinfo.err = x_std_error(&jerr.pub);
    jerr.pub.error_exit = on_error;
    jerr.pub.output_message = on_message;

    if (setjmp(jerr.jump)) {
        if (x_destroy) x_destroy(&cinfo);
        free(row);
        free(out);
        fclose(f);
        return NULL;
    }

    x_create(&cinfo, JPEG_LIB_VERSION, sizeof(struct jpeg_decompress_struct));
    x_stdio_src(&cinfo, f);
    x_read_header(&cinfo, TRUE);

    /* Refuse what will not fit before libjpeg tries to allocate it. A corrupt
     * or absurd SOF is caught by the dimension cap; a progressive image is
     * charged for its full coefficient array, which is the cost scale_denom
     * cannot avoid. */
    if (cinfo.image_width  > COVER_MAX_DIM ||
        cinfo.image_height > COVER_MAX_DIM ||
        cinfo.image_width == 0 || cinfo.image_height == 0)
        longjmp(jerr.jump, 1);

    if (cinfo.progressive_mode) {
        int64_t coeffs = (int64_t)cinfo.image_width * cinfo.image_height *
                         (cinfo.num_components > 0 ? cinfo.num_components : 3) *
                         (int64_t)sizeof(JCOEF);
        if (coeffs > COVER_MEM_BUDGET) longjmp(jerr.jump, 1);
    }

    /* Ask for the smallest decode that still covers the target size; libjpeg
     * supports N/8 scaling and skips most of the work for small denominators. */
    cinfo.scale_num = 1;
    cinfo.scale_denom = 1;
    for (int d = 8; d >= 1; d--) {
        if ((int)(cinfo.image_width / d) >= px && (int)(cinfo.image_height / d) >= px) {
            cinfo.scale_denom = d;
            break;
        }
    }
    cinfo.out_color_space = JCS_RGB;
    cinfo.do_fancy_upsampling = FALSE;
    if (x_calc_dims) x_calc_dims(&cinfo);

    x_start(&cinfo);

    int w = (int)cinfo.output_width, h = (int)cinfo.output_height;
    int comps = cinfo.output_components;
    if (w <= 0 || h <= 0 || comps < 1) longjmp(jerr.jump, 1);

    out = malloc((size_t)px * px * sizeof(uint16_t));
    row = malloc((size_t)w * comps);
    if (!out || !row) longjmp(jerr.jump, 1);

    /* Box-filtered (area-average) down to px*px as scanlines arrive, so the
     * full decoded image never has to be held in memory -- only the source
     * rows contributing to the output row currently being built. This used
     * to be nearest-neighbour (one sample picked per destination pixel,
     * mislabelled as "box" in this same comment) -- cheap, but it visibly
     * aliased fine text and sharp graphic edges on covers, exactly the kind
     * of high-contrast content typography-heavy album art has plenty of,
     * where a photo would have hidden the same shortcut. */
    long *racc = malloc((size_t)w * sizeof(long));
    long *gacc = malloc((size_t)w * sizeof(long));
    long *bacc = malloc((size_t)w * sizeof(long));
    if (!racc || !gacc || !bacc) {
        free(racc); free(gacc); free(bacc);
        longjmp(jerr.jump, 1);
    }

    int next_src_row = 0;
    for (int y = 0; y < px; y++) {
        int row_end = (int)((int64_t)(y + 1) * h / px);
        if (row_end <= next_src_row) row_end = next_src_row + 1;   /* upscaling: >=1 row */
        if (row_end > h) row_end = h;

        memset(racc, 0, (size_t)w * sizeof(long));
        memset(gacc, 0, (size_t)w * sizeof(long));
        memset(bacc, 0, (size_t)w * sizeof(long));
        int rows = 0;
        while (next_src_row < row_end && cinfo.output_scanline < cinfo.output_height) {
            JSAMPROW rp = row;
            x_read_scanlines(&cinfo, &rp, 1);
            for (int sx = 0; sx < w; sx++) {
                const JSAMPLE *p = row + (size_t)sx * comps;
                racc[sx] += p[0];
                gacc[sx] += comps > 1 ? p[1] : p[0];
                bacc[sx] += comps > 2 ? p[2] : p[0];
            }
            next_src_row++;
            rows++;
        }
        if (rows == 0) rows = 1;   /* source exhausted; average of nothing is 0, not a crash */

        for (int x = 0; x < px; x++) {
            int col_start = (int)((int64_t)x * w / px);
            int col_end = (int)((int64_t)(x + 1) * w / px);
            if (col_end <= col_start) col_end = col_start + 1;
            if (col_end > w) col_end = w;
            long rs = 0, gs = 0, bs = 0;
            for (int sx = col_start; sx < col_end; sx++) {
                rs += racc[sx]; gs += gacc[sx]; bs += bacc[sx];
            }
            int n = (col_end - col_start) * rows;
            int r = (int)(rs / n), g = (int)(gs / n), b = (int)(bs / n);
            out[(size_t)y * px + x] =
                (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        }
    }
    free(racc); free(gacc); free(bacc);

    /* Drain anything left so finish_decompress does not complain. */
    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW rp = row;
        x_read_scanlines(&cinfo, &rp, 1);
    }
    x_finish(&cinfo);
    x_destroy(&cinfo);
    free(row);
    fclose(f);

    save_cache(cache_key, px, out);
    return out;
}
