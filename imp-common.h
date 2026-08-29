/*
 * sample-common.h
 *
 * Copyright (C) 2014 Ingenic Semiconductor Co.,Ltd
 */

#ifndef __SAMPLE_COMMON_H__
#define __SAMPLE_COMMON_H__

#include <imp/imp_common.h>
#include <imp/imp_framesource.h>
#include <imp/imp_isp.h>
#include <unistd.h>

#ifdef __cplusplus
#if __cplusplus
extern "C"
{
#endif
#endif /* __cplusplus */

#define SENSOR_FRAME_RATE_NUM		10
#define SENSOR_FRAME_RATE_DEN		1

#define SENSOR_JXF23

#if defined SENSOR_AR0141
#define SENSOR_NAME				"ar0141"
#define SENSOR_CUBS_TYPE        TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR			0x10
#define SENSOR_WIDTH			1280
#define SENSOR_HEIGHT			720
#define CHN0_EN                 1
#define CHN1_EN                 1
#define CROP_EN					1
#elif defined SENSOR_OV7725
#define SENSOR_NAME				"ov7725"
#define SENSOR_CUBS_TYPE        TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR			0x21
#define SENSOR_WIDTH			640
#define SENSOR_HEIGHT			480
#define CHN0_EN                 1
#define CHN1_EN                 0
#define CROP_EN					0
#elif defined SENSOR_OV9732
#define SENSOR_NAME				"ov9732"
#define SENSOR_CUBS_TYPE        TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR			0x36
#define SENSOR_WIDTH			1280
#define SENSOR_HEIGHT			720
#define CHN0_EN                 1
#define CHN1_EN                 1
#define CROP_EN					1
#elif defined SENSOR_OV9750
#define SENSOR_NAME				"ov9750"
#define SENSOR_CUBS_TYPE        TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR			0x36
#define SENSOR_WIDTH			1280
#define SENSOR_HEIGHT			720
#define CHN0_EN                 1
#define CHN1_EN                 1
#define CROP_EN					1
#elif defined SENSOR_OV9712
#define SENSOR_NAME				"ov9712"
#define SENSOR_CUBS_TYPE        TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR			0x30
#define SENSOR_WIDTH			1280
#define SENSOR_HEIGHT			720
#define CHN0_EN                 1
#define CHN1_EN                 1
#define CROP_EN					1
#elif defined SENSOR_GC1004
#define SENSOR_NAME				"gc1004"
#define SENSOR_CUBS_TYPE        TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR			0x3c
#define SENSOR_WIDTH			1280
#define SENSOR_HEIGHT			720
#define CHN0_EN                 1
#define CHN1_EN                 1
#define CROP_EN					1
#elif defined SENSOR_JXH42
#define SENSOR_NAME				"jxh42"
#define SENSOR_CUBS_TYPE        TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR			0x30
#define SENSOR_WIDTH			1280
#define SENSOR_HEIGHT			720
#define CHN0_EN                 1
#define CHN1_EN                 1
#define CROP_EN					1
#elif defined SENSOR_SC1035
#define SENSOR_NAME				"sc1035"
#define SENSOR_CUBS_TYPE        TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR			0x30
#define SENSOR_WIDTH			1280
#define SENSOR_HEIGHT			960
#define CHN0_EN                 1
#define CHN1_EN                 1
#define CROP_EN					1
#elif defined SENSOR_OV2710
#define SENSOR_NAME				"ov2710"
#define SENSOR_CUBS_TYPE        TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR			0x36
#define SENSOR_WIDTH			1920
#define SENSOR_HEIGHT			1080
#define CHN0_EN                 1
#define CHN1_EN                 1
#define CROP_EN					1

#elif defined SENSOR_SC2135
#define SENSOR_NAME				"sc2135"
#define SENSOR_CUBS_TYPE        TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR			0x30
#define SENSOR_WIDTH			1920
#define SENSOR_HEIGHT			1080
#define CHN0_EN                 1
#define CHN1_EN                 1
#define CROP_EN					1
#elif defined SENSOR_OV2735B
#define SENSOR_NAME				"ov2735b"
#define SENSOR_CUBS_TYPE        TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR			0x3c
#define SENSOR_WIDTH			1920
#define SENSOR_HEIGHT			1080
#define CHN0_EN                 1
#define CHN1_EN                 1
#define CROP_EN					1
#elif defined SENSOR_OV2735
#define SENSOR_NAME				"ov2735"
#define SENSOR_CUBS_TYPE        TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR			0x3c
#define SENSOR_WIDTH			1920
#define SENSOR_HEIGHT			1080
#define CHN0_EN                 1
#define CHN1_EN                 1
#define CROP_EN					1
#elif defined SENSOR_JXF22
#define SENSOR_NAME				"jxf22"
#define SENSOR_CUBS_TYPE        TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR			0x40
#define SENSOR_WIDTH			1920
#define SENSOR_HEIGHT			1080
#define CHN0_EN                 1
#define CHN1_EN                 1
#define CROP_EN					1
#elif defined SENSOR_JXF23
#define SENSOR_NAME				"jxf23"
#define SENSOR_CUBS_TYPE        TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDR			0x40
#define SENSOR_WIDTH			1920
#define SENSOR_HEIGHT			1080
#define CHN0_EN                 1
#define CHN1_EN                 0
#define CROP_EN					0
#endif

#define SENSOR_WIDTH_SECOND		640
#define SENSOR_HEIGHT_SECOND		360

#define ENC_H264_CHANNEL		0
#define ENC_JPEG_CHANNEL		2

#define SNAP_FILE_PATH_PREFIX		"/tmp"

#define FS_CHN_NUM			2  //MIN 1,MAX 2

#define CH0_INDEX  0
#define CH1_INDEX  1
#define CHN_ENABLE 1
#define CHN_DISABLE 0

typedef struct{
	/* encoding settings (existing) */
	int ENCODING_TYPE;
	int MAXQP;
	int MINQP;
	int BIASLVL;
	int FROMQPSTEP;
	int GOPQPSTEP;
	double BITRATE;
	int WIDTH;
	int HEIGHT;
	int RATENUM;
	int RATEDEN;
	int PROFILE;
	/* recording settings */
	int recording_enabled;
	char recording_output_dir[256];
	int recording_chunk_duration;
	int recording_disk_threshold;
	/* grafana metrics push settings */
	int grafana_enabled;
	char grafana_push_url[512];
	char grafana_username[64];
	char grafana_api_key[256];
	int grafana_push_interval_ms;
	/* file upload settings */
	int upload_enabled;
	char upload_url[512];
	char upload_token[256];
	int upload_rate_limit_kbps;
	int upload_scan_interval_s;
	int upload_buffer_in_memory;
	int upload_adaptive_rate;
	/* audio settings */
	int audio_enabled;
	int audio_dev_id;        /* 0=digital mic, 1=analog mic */
	int audio_sample_rate;   /* 8000/16000/... (G.711A native) */
	/* http server for snapshots */
	int http_enabled;
	int http_port;           /* default 8080 */
	/* smart encoding mode parameters */
	int SMART_GOP_SEC;	/* GOP length in seconds (I-frame interval); also the
				 * smart-skip cycle on static scenes (1 frame per GOP).
				 * 0 = legacy 2s */
	int SMART_MAXQP;
	int SMART_MINQP;
	int SMART_STATIC_TIME;
	int SMART_QUALITY_LVL;
	int SMART_MAX_BITRATE;
	int SMART_FRM_QPSTEP;
	int SMART_GOP_QPSTEP;
	int SMART_CHANGE_POS;
	/* night mode: runtime encoding overrides (0=disabled) */
	int NIGHT_FPS_NUM;
	int NIGHT_FPS_DEN;
	int NIGHT_BITRATE;
	int NIGHT_MAXQP;
	int NIGHT_QUALITY_LVL;
	/* autonight: built-in photosensitive detection */
	int AUTONIGHT_ENABLED;
	int AUTONIGHT_NIGHT_THRESH;
	int AUTONIGHT_DAY_THRESH;
	int AUTONIGHT_IR_LED_THRESH;
	int AUTONIGHT_IR_LED_OFF;
	int AUTONIGHT_INTERVAL;
} app_config_t;

int app_config_parse(const char *ini_path, app_config_t *config);

struct chn_conf{
	unsigned int index;//0 for main channel ,1 for second channel
	unsigned int enable;
	IMPFSChnAttr fs_chn_attr;
	IMPCell framesource_chn;
	IMPCell imp_encoder;
};

#define  CHN_NUM  ARRAY_SIZE(chn)

int sample_system_init();
int sample_system_exit();

int sample_framesource_streamon();
int sample_framesource_streamoff();

int sample_framesource_init();
int sample_framesource_exit();

void sample_encoder_set_config(app_config_t *config);
int sample_encoder_init();
int sample_jpeg_init();
int sample_encoder_exit(void);

int sample_do_get_jpeg_snap(void);
void *sample_soft_photosensitive_thread(void *p);

#ifdef __cplusplus
extern "C" {
#endif
extern volatile int g_night_mode;
void apply_night_encoding(int night);
#ifdef __cplusplus
}
#endif


#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif /* __cplusplus */

#endif /* __SAMPLE_COMMON_H__ */
