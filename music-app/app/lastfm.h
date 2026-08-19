/* lastfm.h — R23: fall back to Last.fm for an album's cover art when
 * nothing local (embedded picture, folder image) was found. See lastfm.c
 * for the matching logic and why it's shaped the way it is. */
#ifndef LASTFM_H
#define LASTFM_H

/* Reads the API key/secret out of settings.txt once; a no-op on every call
 * after the first (whether or not a key was actually found -- a missing
 * key is a valid, stable answer, not a reason to keep re-reading the
 * file). Safe to call from any thread; only ever actually reads the file
 * once regardless of how many threads call it. */
void lastfm_load_config(void);

/* Whether settings.txt had a usable API key. Calls lastfm_load_config()
 * itself if that hasn't happened yet, so callers don't need to sequence
 * the two. */
int lastfm_has_key(void);

/* Looks up `artist`/`album` (the same album.getinfo call, and the same
 * "largest available image size" pick, Navidrome's Last.fm agent uses)
 * and saves the result to `dest_jpg` on success. Blocking -- does two
 * network round trips via a curl subprocess -- so only ever call this off
 * the UI thread. Returns 0 on success, -1 on any failure (no match, no
 * network, no key, curl missing, decode-unfriendly image -- the caller
 * doesn't need to distinguish these, cover_load() is what actually
 * validates the saved file the next time it's read). */
int lastfm_fetch_cover(const char *artist, const char *album, const char *dest_jpg);

#endif
