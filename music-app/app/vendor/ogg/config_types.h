/* config_types.h — hand-filled for this build.
 *
 * libogg normally generates this from config_types.h.in via its own
 * configure/CMake, substituting the target's fixed-width integer types.
 * Not running that build system here, so it's filled in directly for a
 * standard C99 target (zig cc's mipsel-linux-gnueabihf.2.22, which always
 * has <stdint.h>) rather than guessed — the same values autoconf would have
 * picked for any normal 32/64-bit Linux target. */
#ifndef __CONFIG_TYPES_H__
#define __CONFIG_TYPES_H__

#include <stdint.h>

typedef int16_t  ogg_int16_t;
typedef uint16_t ogg_uint16_t;
typedef int32_t  ogg_int32_t;
typedef uint32_t ogg_uint32_t;
typedef int64_t  ogg_int64_t;
typedef uint64_t ogg_uint64_t;

#endif
