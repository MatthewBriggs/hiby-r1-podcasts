/* mseb.h — persistence for MSEB's 9 gains and its enabled flag. The band
 * definitions themselves (name/Fc/type/Q) are EQ_MSEB_BANDS in eq.h, not
 * here -- this file only ever reads/writes gain_db, the one thing MSEB
 * actually lets the user vary. */
#ifndef MSEB_H
#define MSEB_H

#include "eq.h"

/* Deliberately NOT inside EQProfiles/ -- ep_scan() lists every .txt file in
 * that folder as a selectable profile for the picker on the Parametric EQ
 * screen, and this isn't one: it's not EqualizerAPO format, ep_load() would
 * either fail on it or silently return zero bands, and picking it was never
 * a real option to begin with. A leading dot, matching every other piece of
 * this app's own state on the card (.recent_heard.txt, .podsync, .temp). */
#define MSEB_PATH "/data/mnt/sd_0/.mseb.txt"

/* Missing file or a missing/unrecognised line leaves that gain at 0 dB and
 * enabled at 0 -- a first run is silently flat rather than an error, the
 * same convention lib_group() uses for a track with no tag. */
void mseb_load(float gain_db[MSEB_BAND_N], int *enabled);
int  mseb_save(const float gain_db[MSEB_BAND_N], int enabled);

#endif
