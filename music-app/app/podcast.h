/* podcast.h — feeds, episodes, resume and on-demand download.
 *
 * Ported from the standalone Podcasts app's podcast_hook.c as the data layer
 * for L5 (folding Podcasts into Libra): everything here is business logic
 * and file I/O, no drawing -- music_hook.c owns every screen the same way it
 * already does for audiobook.c's data.
 *
 * Deliberately NOT the SQL index (library.c/h), same reasoning as
 * audiobook.h: the stock scanner does index /Podcasts (that's BG45, which
 * excludes it from Music browsing precisely because it's the wrong shape
 * there -- no episode/feed structure, no resume, no download state), but
 * this module never reads MEDIA_TABLE. It's a plain filesystem walk plus a
 * manifest podsync writes, scoped to /Podcasts, kept entirely separate.
 *
 * Playback itself is not this module's concern -- podcast_hook.c had its
 * own minimp3-based audio.c (MP3 only); this app already has a shared
 * decoder stack in audio.c/h (FLAC/MP3/WAV/M4A/OGG/Opus) that plays podcast
 * files exactly as well, so callers just use audio_play()/audio_pos_ms()/etc
 * from audio.h like anything else in the library.
 *
 * Resume stays its own system, on purpose (explicit decision, not an
 * oversight): audiobooks resume by (file index, ms into that file) because
 * a book is chapters within files; a podcast episode is one file with one
 * position, no chapter structure, and unifying the two formats would gain
 * nothing while adding a migration to get wrong. */
#ifndef PODCAST_H
#define PODCAST_H

#define POD_NAME_LEN  64
#define POD_PATH_LEN  384
#define POD_MAX_ITEMS 220   /* matches the standalone app's own MAX_ITEMS */

typedef struct {
    char name[POD_NAME_LEN];
} pod_feed_t;

typedef struct {
    char name[POD_NAME_LEN];       /* title, extension stripped */
    char path[POD_PATH_LEN];       /* empty if not downloaded */
    char url[POD_PATH_LEN];        /* populated only if not downloaded */
    long mtime;                    /* download order, or the manifest's pubdate */
    int  downloaded;
    int  resume_ms;                /* 0 untouched, POD_FINISHED, else ms */
    int  dur_ms;                   /* 0 if unknown (never true for a downloaded, played episode) */
} pod_episode_t;

#define POD_FINISHED (-1)

/* Feeds are subfolders of /Podcasts with at least one entry (own or
 * manifest-only); scan is name-sorted. */
int pod_scan_feeds(pod_feed_t *out, int max);

/* Episodes of one feed, newest first by mtime -- download order for a local
 * file, parsed pubdate for a manifest-only one. Folds in resume position and
 * duration for anything downloaded. Also remembers `feed` internally as the
 * directory pod_download_start() targets next. */
int pod_load_episodes(const char *feed, pod_episode_t *out, int max);

/* One line per episode: "<ms>\t<dur_ms>\t<path>" in .podsync/resume.txt on
 * the card -- unchanged from the standalone app's format, so an existing
 * resume.txt from that app (or an older release of this one) still reads
 * correctly. ms > 3000 or POD_FINISHED is kept; anything else is dropped,
 * same "don't bother resuming the first few seconds" rule as before. */
void pod_resume_store(const char *path, int ms, int dur);
int  pod_resume_lookup(const char *path, int *dur_out);

/* On-demand download of one manifest-only episode (R18), same fork+curl
 * shape update_start() uses for the whole-feed sync below, scoped to one
 * file. One at a time. `idx` indexes the array pod_load_episodes() last
 * filled. */
void pod_download_start(int idx);
/* Polls the running download. Returns 1 once it completes (ok or not -- call
 * pod_download_ok() to tell which), 0 while still running, -1 if nothing is
 * downloading. Safe to call every frame; cheap when idle. */
int  pod_download_poll(void);
int  pod_download_active(void);
int  pod_download_slot(void);     /* episode index downloading, -1 if none */
long pod_download_bytes(void);
long pod_download_total(void);    /* 0 until Content-Length is known */

/* Whole-feed sync: runs .podsync/podsync_once.sh detached, the same shell
 * fetcher the standalone app used (it already existed and needs the bundled
 * static curl -- busybox wget's TLS is too old for any modern host, and
 * porting that to C would be its own project). */
void pod_update_start(void);
int  pod_update_running(void);
int  pod_update_died(void);
/* Last `max_lines` lines of the fetcher's own log, newest last. Clears
 * pod_update_running() on seeing "__DONE__". Call before pod_update_reap()
 * each tick, same order the standalone app used, so a completion marker
 * written just as the child exits is still seen. */
int  pod_update_tail(char out[][POD_NAME_LEN], int max_lines);
/* Reaps the fetcher's pid once it has exited; marks pod_update_died() if it
 * exited without ever writing "__DONE__" (killed, or exec failed). */
void pod_update_reap(void);

/* The episode's notes sidecar (same basename, .txt), raw and unwrapped --
 * wrapping to the screen width is a drawing concern, left to the caller. 0
 * if there's no sidecar or it's empty. */
int pod_load_notes(const char *audio_path, char *out, int max_len);

#endif
