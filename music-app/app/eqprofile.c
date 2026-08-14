/* eqprofile.c — see eqprofile.h. */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "eqprofile.h"

#define EP_DIR "/data/mnt/sd_0/EQProfiles"

static int cmp_name(const void *a, const void *b) {
    return strcasecmp(((const ep_entry_t *)a)->name, ((const ep_entry_t *)b)->name);
}

int ep_scan(ep_entry_t *out, int max) {
    mkdir(EP_DIR, 0777);
    DIR *d = opendir(EP_DIR);
    if (!d) return 0;
    struct dirent *e;
    int n = 0;
    while (n < max && (e = readdir(d))) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcasecmp(dot, ".txt") != 0) continue;
        snprintf(out[n].path, sizeof(out[n].path), "%s/%s", EP_DIR, e->d_name);
        size_t len = (size_t)(dot - e->d_name);
        if (len >= sizeof(out[n].name)) len = sizeof(out[n].name) - 1;
        memcpy(out[n].name, e->d_name, len);
        out[n].name[len] = '\0';
        n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof(out[0]), cmp_name);
    return n;
}

int ep_load(const char *path, eq_profile_t *out) {
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(out->name, sizeof(out->name), "%s", base);
    char *dot = strrchr(out->name, '.');
    if (dot) *dot = '\0';

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        float preamp;
        if (sscanf(line, "Preamp: %f dB", &preamp) == 1) {
            out->preamp_db = preamp;
            continue;
        }
        int idx; char onoff[8], type[8];
        float fc, gain, q;
        if (sscanf(line, "Filter %d: %7s %7s Fc %f Hz Gain %f dB Q %f",
                   &idx, onoff, type, &fc, &gain, &q) != 6)
            continue;
        eq_type_t t;
        if (!strcasecmp(type, "PK"))       t = EQ_PEAK;
        else if (!strcasecmp(type, "LSC")) t = EQ_LOW_SHELF;
        else if (!strcasecmp(type, "HSC")) t = EQ_HIGH_SHELF;
        else continue;   /* HPF/LPF/NO/AP and friends: unsupported, skipped
                          * rather than misread as one of the three we do. */
        if (out->band_n >= EQ_MAX_BANDS) continue;
        eq_band_t *b = &out->band[out->band_n++];
        b->type = t;
        b->on = !strcasecmp(onoff, "ON");
        b->fc = fc;
        b->gain_db = gain;
        b->q = q;
    }
    fclose(f);
    return out->band_n;
}

int ep_save(const char *path, const eq_profile_t *p) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "Preamp: %.2f dB\n", p->preamp_db);
    for (int i = 0; i < p->band_n; i++) {
        const eq_band_t *b = &p->band[i];
        const char *type = b->type == EQ_PEAK ? "PK" :
                          b->type == EQ_LOW_SHELF ? "LSC" : "HSC";
        fprintf(f, "Filter %d: %s %s Fc %.1f Hz Gain %.1f dB Q %.2f\n",
               i + 1, b->on ? "ON" : "OFF", type, b->fc, b->gain_db, b->q);
    }
    fclose(f);
    return 0;
}
