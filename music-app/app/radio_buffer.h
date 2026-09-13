#ifndef RADIO_BUFFER_H
#define RADIO_BUFFER_H

#include <stddef.h>
#include <stdint.h>

/* Radio time-shift buffer: a background thread drains the live source (a
 * direct MP3 stream, or an HLS playlist demuxed to ADTS) into chunk files on
 * the SD card at whatever rate it actually arrives, independent of where
 * playback currently is. Playback reads from this buffer at a byte cursor
 * that can sit anywhere in the retained window -- at the live edge for
 * normal listening, or behind it for pause/rewind. Only the station
 * currently playing is ever buffered; starting a new station tears down and
 * replaces the previous one's chunks.
 *
 * Byte position stands in for time: internet radio is effectively CBR per
 * station, and a scrub UI does not need frame-accurate seeking, so a running
 * bytes-per-second estimate (measured from real arrival rate, not assumed
 * from a nominal bitrate) is enough to convert between the two.
 */

typedef enum {
    RB_KIND_MP3 = 0,   /* raw MP3 stream bytes, as arrives */
    RB_KIND_ADTS = 1,  /* AAC access units in ADTS framing, post HLS/TS demux */
} rb_kind_t;

/* Begin a fresh buffer session for this station. Wipes any previous
 * session's chunk files first. url's scheme (http/https + .m3u8 extension)
 * decides HLS vs direct-MP3 internally, same detection audio.c's own
 * dec_open() already does -- pass which one the caller already resolved.
 * Returns 0 on success (background thread is running), -1 if the fetch
 * could not even start (bad URL, no response).
 */
int rb_start(const char *url, rb_kind_t kind);

/* Stop the background thread and close files. Chunk files are left on disk
 * until the next rb_start() wipes them, so a rb_stop() immediately followed
 * by rb_start() on the same station (a pause/resume that happens to also
 * restart the session) would lose nothing if it reused them -- not done
 * today, kept simple, since pausing does not call this at all (see below). */
void rb_stop(void);

/* Total bytes ever written this session -- the live edge. Reading at this
 * position gets nothing yet (not written); anything below it is available
 * down to rb_oldest(). */
uint64_t rb_live_pos(void);

/* Earliest byte position still on disk (older chunks are evicted once the
 * retained window exceeds RB_WINDOW_SECONDS of estimated playback time). */
uint64_t rb_oldest_pos(void);

/* Read up to len bytes starting at byte position pos (may span more than one
 * chunk file). Returns bytes actually read, 0 if pos is at or past the live
 * edge (nothing there yet) or before rb_oldest_pos() (evicted). */
size_t rb_read_at(uint64_t pos, unsigned char *out, size_t len);

/* Running estimate of the station's real arrival rate, bytes/sec, measured
 * from the background thread's own write rate rather than assumed from a
 * nominal bitrate (which the source is not obligated to honour exactly).
 * 0 until enough data has arrived to measure. */
double rb_bytes_per_sec(void);

/* 1 if the background fetch thread is alive and this session's kind. */
int rb_active(void);
rb_kind_t rb_current_kind(void);

/* Recording: taps the exact same bytes the background thread is already
 * writing into chunk files (see rb_write_bytes() in radio_buffer.c) and
 * copies them, as-is, into a second, permanent file for as long as
 * recording is active -- no transcoding, no separate fetch. Starts writing
 * from whatever arrives *after* this call, not from anything already
 * buffered (the time-shift window and a recording are two different
 * lifetimes: one is a rolling 30 minutes that exists whether or not anyone
 * asked for it, the other is exactly what a listener chose to keep).
 * path's extension should match rb_current_kind() (.mp3 for RB_KIND_MP3,
 * .aac for RB_KIND_ADTS -- audio.c's local-file playback dispatches on it)
 * so the recording is actually listenable afterward.
 * Returns 0 on success, -1 if the file could not be opened or no session
 * is active. */
int rb_recording_start(const char *path);

/* Stop and close the recording (if any). Safe to call when not recording. */
void rb_recording_stop(void);

int rb_is_recording(void);

#endif
