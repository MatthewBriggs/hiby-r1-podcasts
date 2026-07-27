/* audio.h — MP3 playback for the R1 podcast app. */
#ifndef AUDIO_H
#define AUDIO_H

int  audio_init(void);              /* dlopen ALSA; 0 on success */
int  audio_play(const char *path, int start_ms);
void audio_toggle_pause(void);
void audio_stop(void);
void audio_seek_relative(int delta_ms);

int  audio_is_active(void);         /* a file is loaded */
int  audio_is_paused(void);
int  audio_position_ms(void);
int  audio_duration_ms(void);
const char *audio_error(void);      /* NULL when fine */

#endif /* AUDIO_H */
