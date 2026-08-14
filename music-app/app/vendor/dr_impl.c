/* dr_impl.c — the dr_libs implementations, isolated in one translation unit so
 * the rest of the app includes only their declarations.
 *
 * These are bundled rather than dlopen'd from the device because the device's
 * libsndfile is built without libFLAC: it links libc and libm and nothing else,
 * and returns "unimplemented format" for FLAC, which is 83% of the library.
 */
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_OGG            /* no Ogg-FLAC in the library; saves ~40 KB */
#include "dr_flac.h"

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
