/* audio.h — playback. Decoders are dlopen'd from the device; see audio.c. */
#ifndef AUDIO_H
#define AUDIO_H
int  audio_play(const char *path);
void audio_stop(void);
void audio_toggle(void);
void audio_seek_ms(int ms);
/* Pitch-preserving playback speed (WSOLA), permille: 1000 = 1.0x, clamped to
 * [800,2000]. 1000 bypasses WSOLA entirely -- exact passthrough, same as
 * before this existed. Applies to the 16-bit output path only; a hires
 * source keeps playing at 1.0x regardless (audiobooks are never hires in
 * practice, so this is not a real-world limitation). */
void audio_set_speed(int permille);
int  audio_speed(void);

/* Gapless: hand over the track to roll into when this one ends. The worker
 * takes it at the boundary without closing the output device.
 * audio_take_advance reports (and clears) how many times that has happened, so
 * the UI can follow along and queue the one after. */
void audio_set_next(const char *path);
int  audio_take_advance(void);
int  audio_is_active(void);
int  audio_is_paused(void);
int  audio_pos_ms(void);
int  audio_dur_ms(void);
void audio_set_volume(int pct);
int  audio_volume(void);
void audio_volume_step(int delta);   /* routes to the BT mixer or software */
void audio_volume_set(int pct);      /* absolute, for the slider */
int  audio_using_bt(void);
/* Background-thread only (bt_poll) -- both do blocking D-Bus round-trips via
 * amixer. audio_bt_volume_pending() is the cheap check between polls;
 * audio_bt_volume_service() applies a pending write (from a key or the
 * slider) and always reads back afterward. Neither may run on the thread
 * that also draws frames and reads input -- see the comment above
 * audio_volume_set() in audio.c for why. */
int  audio_bt_volume_pending(void);
void audio_bt_volume_service(void);
const char *audio_codec(void);        /* of the stream now playing */
int  audio_is_exact(void);            /* output opened with no conversion layer */
int  audio_output_lost(void);         /* playback stopped because the device went away */
const char *audio_output(void);   /* "3.5 mm", "USB" or "Bluetooth" */

/* R28: header-only duration probe for a track the library's own database
 * has no duration for (see the comment above its definition in audio.c for
 * why that is the common case, not a rare one). bitrate_bps is only used
 * as an MP3/VBR fallback; pass 0 if unknown. Safe to call from any thread,
 * including while something else is playing. Returns 0 on failure. */
int audio_probe_dur_ms(const char *path, int bitrate_bps);
#endif
