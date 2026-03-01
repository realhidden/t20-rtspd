#ifndef __MKV_RECORDER_H__
#define __MKV_RECORDER_H__

#include <stdint.h>
#include <imp/imp_common.h>
#include <imp/imp_encoder.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int enabled;
	char output_dir[256];
	int chunk_duration;        /* seconds */
	int disk_usage_threshold;  /* percent (0-100) */
	int width;
	int height;
	int fps_num;
	int fps_den;
} mkv_recorder_config_t;

int mkv_recorder_init(const mkv_recorder_config_t *config);
int mkv_recorder_write_frame(IMPEncoderStream *stream);
void mkv_recorder_shutdown(void);
int64_t mkv_recorder_get_frame_count(void);
int64_t mkv_recorder_get_chunk_start_time(void);
int mkv_recorder_get_disk_usage(int *usage_pct, unsigned long *free_kb);

#ifdef __cplusplus
}
#endif

#endif /* __MKV_RECORDER_H__ */
