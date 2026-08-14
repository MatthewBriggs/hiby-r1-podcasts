/* jconfig.h — minimal configuration for libjpeg 9 on Linux/MIPS.
 * Only declarations are needed: the library itself is dlopen'd from the device
 * (/usr/lib/libjpeg.so.9), so this must describe the same ABI, not build it. */
#define HAVE_PROTOTYPES 1
#define HAVE_UNSIGNED_CHAR 1
#define HAVE_UNSIGNED_SHORT 1
#define HAVE_STDDEF_H 1
#define HAVE_STDLIB_H 1
#undef void
#undef const
#undef CHAR_IS_UNSIGNED
#undef NEED_BSD_STRINGS
#undef NEED_SYS_TYPES_H
#undef NEED_FAR_POINTERS
#undef NEED_SHORT_EXTERNAL_NAMES
#undef INCOMPLETE_TYPES_BROKEN
