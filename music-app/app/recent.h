/* recent.h — "last heard" timestamps per album, for R30.
 *
 * "Recently added" needs no tracking of our own: MEDIA_TABLE already carries
 * ctime/mtime per track (see library.c's lib_albums_recent_added()). "Recently
 * heard" has no such column and no stock equivalent this app can safely read
 * -- the obvious candidate, the card's own most_played.db, has an unknown,
 * undocumented schema, and finding out costs a blind filesystem crawl on a
 * single-core device for a payoff that is not worth the risk. This file is
 * the safer alternative: track it ourselves, from the one place a track
 * actually starts playing. */
#ifndef RECENT_H
#define RECENT_H

#define RECENT_HEARD_PATH "/data/mnt/sd_0/.recent_heard.txt"
/* Only the most recent handful of albums are ever relevant to a top-10 list,
 * so this does not need to hold the whole library -- 32 is generous
 * headroom, not a real limit; the oldest entry is evicted once it fills. */
#define RECENT_HEARD_MAX 32

void recent_heard_load(void);
/* Call whenever a track actually starts playing, with the album it belongs
 * to. Updates the in-memory table and rewrites RECENT_HEARD_PATH -- rare
 * enough (once per track change, not once per frame) that a full rewrite of
 * a few dozen short lines costs nothing worth avoiding. */
void recent_heard_mark(const char *album);
/* 0 if this album has never been marked. */
long long recent_heard_ts(const char *album);

#endif
