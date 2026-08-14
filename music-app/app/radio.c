/* radio.c — the station list.
 *
 * Stations live in a plain "Name | URL" text file rather than being compiled
 * in, because which stations work is not something this app can decide. Two
 * kinds play: a direct MP3 stream, fed straight to the decoder, and an HLS
 * playlist, whose segments are demuxed from MPEG-TS and decoded as AAC.
 *
 * On the BBC: their live radio is served through an endpoint that describes
 * itself as part of a content protection system, so this app does not go
 * looking for URLs there. Paste one in and it plays like any other — the
 * player has no opinion about where a URL came from.
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "radio.h"

#define STATIONS_PATH "/usr/data/radio_stations.conf"

/* Written on first run so the file exists to be edited, rather than the user
 * having to guess the format. These are stations that publish a direct stream
 * URL openly. */
static const char *seed =
    "# One station per line:  Name | URL\n"
    "# Direct MP3 streams and HLS playlists (.m3u8) both work.\n"
    "# Lines starting with # are ignored.\n"
    "NRK Klassisk | https://nrk-live-radio-world.akamaized.net/klassisk/muxed.m3u8?adap=audio&aco=aac\n"
    "rbbKultur | https://dispatcher.rndfnk.com/rbb/rbbkultur/live/mp3/high\n"
    "BBC Radio 3 | \n"
    "BBC Radio 4 | \n";

static void trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                 s[n - 1] == ' '  || s[n - 1] == '\t')) s[--n] = '\0';
}

int radio_load(radio_station_t *out, int max) {
    FILE *f = fopen(STATIONS_PATH, "r");
    if (!f) {
        f = fopen(STATIONS_PATH, "w");
        if (f) { fputs(seed, f); fclose(f); }
        f = fopen(STATIONS_PATH, "r");
        if (!f) return 0;
    }

    char line[768];
    int n = 0;
    while (n < max && fgets(line, sizeof(line), f)) {
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        char *bar = strchr(line, '|');
        if (!bar) continue;
        *bar = '\0';
        char *url = bar + 1;
        trim(line);
        trim(url);
        /* A station with no URL yet is kept and shown greyed rather than
         * dropped — it is a slot the user has left themselves. */
        if (!line[0]) continue;
        snprintf(out[n].name, sizeof(out[n].name), "%s", line);
        snprintf(out[n].url,  sizeof(out[n].url),  "%s", url);
        n++;
    }
    fclose(f);
    return n;
}
