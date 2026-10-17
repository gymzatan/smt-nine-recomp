/**
 * XAudio2 Audio Output Backend
 */
#ifndef APU_XAUDIO2_H
#define APU_XAUDIO2_H

#include <stdint.h>
#include <stdio.h>

/* Initialize XAudio2. Returns 1 on success, 0 on failure. */
int xa2_init(void);

/* Shut down XAudio2 and release all resources. */
void xa2_shutdown(void);

/* Returns 1 if XAudio2 is active. */
int xa2_is_active(void);

/* Submit interleaved stereo 16-bit samples. Returns 1 if accepted. */
int xa2_submit_samples(const int16_t *samples, int num_samples);

/* Get the preferred buffer size in samples. */
int xa2_get_buffer_size(void);
/* Remaining submitted PCM bytes, from the actual source voice sample cursor. */
int xa2_queued_bytes(void);
void xa2_checkpoint_pause(void);
void xa2_checkpoint_resume(void);
int xa2_save_checkpoint(FILE *);
void *xa2_prepare_checkpoint(FILE *);
int xa2_apply_checkpoint(const void *);

#endif /* APU_XAUDIO2_H */
