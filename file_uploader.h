#ifndef __FILE_UPLOADER_H__
#define __FILE_UPLOADER_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int enabled;
	char upload_url[512];
	char upload_token[256];
	char scan_dir[256];
	int rate_limit_kbps;
	int scan_interval_s;
	int buffer_in_memory;
	int adaptive_rate;        /* 1 = enable adaptive rate limiting */
} file_uploader_config_t;

int file_uploader_init(const file_uploader_config_t *config);
void file_uploader_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* __FILE_UPLOADER_H__ */
