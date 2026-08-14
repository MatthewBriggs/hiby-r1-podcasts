/* wps.h — Rockbox "While Playing Screen" themes.
 *
 * A theme is a .cfg naming a .wps, plus the images it references. The .wps is
 * a small tag language: literal text, %tags, %?conditionals<a|b|c>, and
 * viewports that group lines into rectangles. This parses one into a tree and
 * renders it against playback state the host app supplies, so nothing in here
 * knows about this player's internals -- and the renderer never reads a file,
 * so a theme cannot stall the UI thread on SD-card I/O the way BG27 describes.
 *
 * Deliberately partial. The language has far more tags than any one theme
 * uses, so unknown tags render as nothing and unknown conditionals take their
 * false branch, which degrades a theme gracefully instead of refusing it --
 * the same choice art_candidate() makes about art it cannot decode.
 */
#ifndef WPS_H
#define WPS_H

#include <stdint.h>
#include <stddef.h>

#define WPS_MAX_IMAGES    32
#define WPS_MAX_VIEWPORTS 24
#define WPS_MAX_LINES    128
#define WPS_MAX_TOUCH     32
#define WPS_MAX_TOKENS    24
/* cabbiev2's play-mode selector has nine branches (%?mp with five images and
 * four empty slots), and a cap below that leaves the tail of the conditional
 * unconsumed -- the leftover "|>" then renders as literal text on screen. */
#define WPS_MAX_BRANCH    16

/* What the host must tell the renderer to draw a frame. Strings may be NULL;
 * the renderer treats NULL and "" alike so a caller never has to invent
 * placeholder text. */
typedef struct {
    const char *title, *artist, *album, *albumartist, *genre, *year, *filename;
    const char *codec;
    int  pos_ms, dur_ms;
    int  track_no, track_count;      /* playlist position, 1-based */
    int  volume_pct;
    int  playing;                    /* 0 = paused */
    int  shuffle;
    int  bitrate_kbps;
    const uint16_t *art;             /* square RGB565 cover, or NULL */
    int  art_px;
} wps_state;

typedef struct wps_theme wps_theme;

/* Loads a theme by its .cfg path. `root` is the directory the theme's
 * absolute /.rockbox/... paths resolve against (i.e. the card mount point).
 * Returns NULL if the .cfg or its .wps cannot be read. */
wps_theme *wps_load(const char *cfg_path, const char *root);
void       wps_free(wps_theme *t);

/* Draws the whole screen, backdrop included. fb is FB_W*FB_H RGB565. */
void wps_render(wps_theme *t, uint16_t *fb, int fb_w, int fb_h, const wps_state *st);

/* The action name of the touch region under (x,y), or NULL. Region names are
 * Rockbox's own ("play", "next", "prev", "volume", ...) and are passed through
 * verbatim for the host to interpret -- this layer has no opinion about what
 * they should do. */
const char *wps_hit(const wps_theme *t, int x, int y);

/* Scans `dir` for .cfg files, filling `names` with their basenames. Returns
 * the count, capped at max. */
int wps_scan_themes(const char *dir, char names[][64], int max);

#endif
