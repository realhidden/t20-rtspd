#ifndef __AUDIO_CAPTURE_H__
#define __AUDIO_CAPTURE_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int enabled;
	int dev_id;       /* 0=digital mic, 1=analog mic */
	int sample_rate;  /* 8000/16000/... (G.711A native) */
	int channels;     /* 1=mono, 2=stereo */
} audio_capture_config_t;

/* Initialize IMP audio-in + G.711A encoder and start the capture thread.
 * Each encoded frame is handed to mkv_recorder_write_audio_frame().
 * Returns 0 on success. No-op (returns 0) if config->enabled is 0. */
int audio_capture_start(const audio_capture_config_t *config);

/* Signal the capture thread to stop, join it, and tear down IMP audio.
 * Safe to call if audio_capture_start() was never called or failed. */
void audio_capture_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_CAPTURE_H__ */
