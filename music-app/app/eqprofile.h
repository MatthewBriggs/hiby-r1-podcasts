/* eqprofile.h — EqualizerAPO-format profiles on the card. Same shape as
 * playlist.c's .m3u handling: plain text files in one folder, so a profile
 * can be dropped on by computer -- via GraphicEQ tools, AutoEq, or by hand --
 * and read straight off the card, nothing locked inside a database. */
#ifndef EQPROFILE_H
#define EQPROFILE_H

#include "eq.h"

#define EP_NAME_LEN 64
#define EP_PATH_LEN 384
#define EP_MAX_PROFILES 64

typedef struct {
    char name[EP_NAME_LEN];   /* filename without .txt */
    char path[EP_PATH_LEN];
} ep_entry_t;

/* Lists the .txt files under EQProfiles, name-sorted. Creates the folder on
 * first use. */
int ep_scan(ep_entry_t *out, int max);

/* Parses "Preamp: X dB" and "Filter N: ON|OFF TYPE Fc X Hz Gain X dB Q X"
 * lines -- the EqualizerAPO ParametricEq format. TYPE is LSC/PK/HSC; any
 * other filter type (HPF, LPF, NO, AP...) is skipped rather than
 * misinterpreted, since only those three are supported. Returns the number
 * of bands read (0 on total failure -- no file, or nothing recognised). */
int ep_load(const char *path, eq_profile_t *out);

/* Writes the same format back. Used to persist a live edit (a dragged
 * slider) into the profile it came from, so it survives a restart. */
int ep_save(const char *path, const eq_profile_t *p);

#endif
