/* recent.c — see recent.h. */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "library.h"
#include "recent.h"

typedef struct { char album[LIB_NAME_LEN]; long long ts; } entry_t;

static entry_t g_ent[RECENT_HEARD_MAX];
static int     g_n;

static void save(void) {
    FILE *f = fopen(RECENT_HEARD_PATH, "w");
    if (!f) return;
    for (int i = 0; i < g_n; i++)
        fprintf(f, "%lld %s\n", g_ent[i].ts, g_ent[i].album);
    fclose(f);
}

void recent_heard_load(void) {
    g_n = 0;
    FILE *f = fopen(RECENT_HEARD_PATH, "r");
    if (!f) return;
    char line[256];
    while (g_n < RECENT_HEARD_MAX && fgets(line, sizeof(line), f)) {
        long long ts; int off = 0;
        if (sscanf(line, "%lld %n", &ts, &off) != 1 || off <= 0) continue;
        char *album = line + off;
        size_t len = strlen(album);
        while (len > 0 && (album[len - 1] == '\n' || album[len - 1] == '\r')) len--;
        album[len] = '\0';
        if (!len) continue;
        snprintf(g_ent[g_n].album, sizeof(g_ent[g_n].album), "%s", album);
        g_ent[g_n].ts = ts;
        g_n++;
    }
    fclose(f);
}

void recent_heard_mark(const char *album) {
    if (!album || !album[0]) return;
    long long now = (long long)time(NULL);
    for (int i = 0; i < g_n; i++)
        if (!strcmp(g_ent[i].album, album)) { g_ent[i].ts = now; save(); return; }
    if (g_n < RECENT_HEARD_MAX) {
        snprintf(g_ent[g_n].album, sizeof(g_ent[g_n].album), "%s", album);
        g_ent[g_n].ts = now;
        g_n++;
    } else {
        /* Full: evict whichever entry is already the oldest, same as an LRU
         * cache would -- it is, by definition, the one least likely to still
         * be "recent" by the time this table is read. */
        int oldest = 0;
        for (int i = 1; i < g_n; i++)
            if (g_ent[i].ts < g_ent[oldest].ts) oldest = i;
        snprintf(g_ent[oldest].album, sizeof(g_ent[oldest].album), "%s", album);
        g_ent[oldest].ts = now;
    }
    save();
}

long long recent_heard_ts(const char *album) {
    if (!album) return 0;
    for (int i = 0; i < g_n; i++)
        if (!strcmp(g_ent[i].album, album)) return g_ent[i].ts;
    return 0;
}
