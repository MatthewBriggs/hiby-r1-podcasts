/* lastfm.c — see lastfm.h. R23: album art fallback when nothing local
 * (embedded picture, folder image) exists for an album, matching the same
 * shape Navidrome's own Last.fm agent uses: an unauthenticated
 * album.getinfo call by artist+album, taking the largest available image
 * size from the response. No request signing needed -- that's only
 * required for authenticated calls (scrobbling), not a plain read.
 *
 * Network access goes through the same bundled, TLS-capable curl the
 * podcast sync script already carries (busybox's own wget/curl has TLS too
 * old for HTTPS -- see podcast.c's own note on this), so this adds no new
 * dependency; if that curl isn't present (podcasts never set up), a system
 * curl on PATH is tried as a fallback, and failing that this just quietly
 * does nothing, the same as any other missing-art case. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lastfm.h"

#define SETTINGS_PATH   "/data/mnt/sd_0/settings.txt"
#define CURL_BIN        "/data/mnt/sd_0/.podsync/curl"
#define CURL_CA         "/data/mnt/sd_0/.podsync/cacert.pem"

static char g_api_key[64];
static char g_secret[64];   /* not currently used -- getinfo needs no signing -- kept for a future authenticated call */
static int  g_loaded;

void lastfm_load_config(void) {
    if (g_loaded) return;
    g_loaded = 1;
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (!f) return;
    char line[256];
    int in_section = 0, want = 0;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        char *cr = strchr(line, '\r'); if (cr) *cr = '\0';
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!in_section) {
            if (!strcmp(s, "LastFM")) in_section = 1;
            continue;
        }
        if (s[0] == '\0' || s[0] == '#') continue;
        if (want == 0) { snprintf(g_api_key, sizeof(g_api_key), "%s", s); want = 1; }
        else { snprintf(g_secret, sizeof(g_secret), "%s", s); break; }
    }
    fclose(f);
}

int lastfm_has_key(void) {
    lastfm_load_config();
    return g_api_key[0] != '\0';
}

static void url_encode(const char *in, char *out, size_t out_n) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 4 < out_n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[j++] = (char)c;
        } else {
            snprintf(out + j, out_n - j, "%%%02X", c);
            j += 3;
        }
    }
    out[j] = '\0';
}

/* Fetches `url` to `out_path` via curl, bundled copy first. A plain
 * fork+exec with the URL as one argv element -- never a shell, so nothing
 * in an artist or album name (quotes, $, backticks, whatever) can be
 * interpreted as anything but literal bytes of the URL. */
static int run_curl(const char *url, const char *out_path) {
    unlink(out_path);
    pid_t pid = fork();
    if (pid == 0) {
        execl(CURL_BIN, CURL_BIN, "-fsSL", "--cacert", CURL_CA,
              "--connect-timeout", "10", "--max-time", "20",
              "-o", out_path, url, (char *)NULL);
        /* Bundled curl missing (podcasts never set up on this device) --
         * try whatever's on PATH before giving up. */
        execlp("curl", "curl", "-fsSL",
               "--connect-timeout", "10", "--max-time", "20",
               "-o", out_path, url, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return -1;
    int status;
    if (waitpid(pid, &status, 0) != pid) return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    struct stat st;
    if (stat(out_path, &st) != 0 || st.st_size <= 0) return -1;
    return 0;
}

/* Last.fm's own image array runs small -> mega, and a size that wasn't
 * generated comes back as an empty "#text" rather than being omitted, so
 * the last non-empty #text within the array is the largest one actually
 * available -- scanned directly rather than through a JSON parser, the
 * same "just enough" approach this codebase already uses for MP4 atoms
 * and ID3 frames. Stops at the array's own closing ']' so a later,
 * unrelated field can never be mistaken for another image entry. */
static int extract_best_image_url(const char *buf, char *out, size_t out_n) {
    const char *arr = strstr(buf, "\"image\"");
    if (!arr) return -1;
    arr = strchr(arr, '[');
    if (!arr) return -1;
    const char *end = strchr(arr, ']');
    if (!end) end = arr + strlen(arr);
    out[0] = '\0';
    const char *p = arr;
    while (p < end) {
        const char *k = strstr(p, "\"#text\"");
        if (!k || k >= end) break;
        const char *c = strchr(k, ':');
        if (!c || c >= end) break;
        const char *q1 = strchr(c, '"');
        if (!q1 || q1 >= end) break;
        const char *q2 = strchr(q1 + 1, '"');
        if (!q2) break;
        size_t len = (size_t)(q2 - q1 - 1);
        if (len > 0 && len < out_n) {
            memcpy(out, q1 + 1, len);
            out[len] = '\0';
        }
        p = q2 + 1;
    }
    return out[0] ? 0 : -1;
}

int lastfm_fetch_cover(const char *artist, const char *album, const char *dest_jpg) {
    lastfm_load_config();
    if (!g_api_key[0] || !artist[0] || !album[0]) return -1;

    char ea[300], eb[300];
    url_encode(artist, ea, sizeof(ea));
    url_encode(album, eb, sizeof(eb));

    char url[900];
    snprintf(url, sizeof(url),
             "https://ws.audioscrobbler.com/2.0/?method=album.getinfo"
             "&api_key=%s&artist=%s&album=%s&format=json",
             g_api_key, ea, eb);

    char meta_path[64];
    snprintf(meta_path, sizeof(meta_path), "/tmp/.lastfm_meta_%d.json", (int)getpid());
    if (run_curl(url, meta_path) != 0) { unlink(meta_path); return -1; }

    FILE *f = fopen(meta_path, "rb");
    if (!f) { unlink(meta_path); return -1; }
    char *buf = malloc(65536);
    size_t n = buf ? fread(buf, 1, 65535, f) : 0;
    fclose(f);
    unlink(meta_path);
    if (!buf) return -1;
    buf[n] = '\0';

    char img_url[600];
    int rc = extract_best_image_url(buf, img_url, sizeof(img_url));
    free(buf);
    if (rc != 0) return -1;

    char tmp_jpg[300];
    snprintf(tmp_jpg, sizeof(tmp_jpg), "%s.part", dest_jpg);
    if (run_curl(img_url, tmp_jpg) != 0) { unlink(tmp_jpg); return -1; }
    if (rename(tmp_jpg, dest_jpg) != 0) { unlink(tmp_jpg); return -1; }
    return 0;
}
