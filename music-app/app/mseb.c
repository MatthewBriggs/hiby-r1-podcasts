/* mseb.c — see mseb.h. Plain text, one "Name: X.X dB" line per band plus an
 * Enabled line, matched against EQ_MSEB_BANDS[i].name rather than position --
 * order in the file doesn't matter and an old file missing a band (the table
 * gained one, say) just leaves that gain at 0 rather than misreading the
 * next line as it. */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "mseb.h"

void mseb_load(float gain_db[MSEB_BAND_N], int *enabled) {
    memset(gain_db, 0, sizeof(gain_db[0]) * MSEB_BAND_N);
    *enabled = 0;
    FILE *f = fopen(MSEB_PATH, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int v;
        if (sscanf(line, "Enabled: %d", &v) == 1) { *enabled = v != 0; continue; }
        for (int i = 0; i < MSEB_BAND_N; i++) {
            size_t nlen = strlen(EQ_MSEB_BANDS[i].name);
            if (strncmp(line, EQ_MSEB_BANDS[i].name, nlen) != 0) continue;
            if (line[nlen] != ':') continue;
            float g;
            if (sscanf(line + nlen + 1, "%f", &g) == 1) gain_db[i] = g;
            break;
        }
    }
    fclose(f);
}

int mseb_save(const float gain_db[MSEB_BAND_N], int enabled) {
    mkdir("/data/mnt/sd_0/EQProfiles", 0777);
    FILE *f = fopen(MSEB_PATH, "w");
    if (!f) return -1;
    fprintf(f, "Enabled: %d\n", enabled ? 1 : 0);
    for (int i = 0; i < MSEB_BAND_N; i++)
        fprintf(f, "%s: %.1f dB\n", EQ_MSEB_BANDS[i].name, gain_db[i]);
    fclose(f);
    return 0;
}
