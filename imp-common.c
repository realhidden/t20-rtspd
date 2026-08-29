#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>

#include <imp/imp_log.h>
#include <imp/imp_common.h>
#include <imp/imp_system.h>
#include <imp/imp_framesource.h>
#include <imp/imp_encoder.h>
#include <imp/imp_isp.h>
#include <ini.h>

#include "imp-common.h"

#define TAG "imp-Common"

struct chn_conf chn[FS_CHN_NUM] = {
	{
		.index = CH0_INDEX,
		.enable = CHN0_EN,
		.fs_chn_attr = {
			.pixFmt = PIX_FMT_NV12,
			.outFrmRateNum = SENSOR_FRAME_RATE_NUM,
			.outFrmRateDen = SENSOR_FRAME_RATE_DEN,
			.nrVBs = 2,
			.type = FS_PHY_CHANNEL,

			.crop.enable = CROP_EN,
			.crop.top = 0,
			.crop.left = 0,
			.crop.width = SENSOR_WIDTH,
			.crop.height = SENSOR_HEIGHT,

			.scaler.enable = 0,

			.picWidth = SENSOR_WIDTH,
			.picHeight = SENSOR_HEIGHT,
		   },
		.framesource_chn =	{ DEV_ID_FS, 0, 0},
		.imp_encoder = { DEV_ID_ENC, 0, 0},
	},
	{
		.index = CH1_INDEX,
		.enable = CHN1_EN,
		.fs_chn_attr = {
			.pixFmt = PIX_FMT_NV12,
			.outFrmRateNum = SENSOR_FRAME_RATE_NUM,
			.outFrmRateDen = SENSOR_FRAME_RATE_DEN,
			.nrVBs = 2,
			.type = FS_PHY_CHANNEL,

			.crop.enable = CROP_EN,
			.crop.top = 0,
			.crop.left = 0,
			.crop.width = SENSOR_WIDTH,
			.crop.height = SENSOR_HEIGHT,

			.scaler.enable = 1,
			.scaler.outwidth = SENSOR_WIDTH_SECOND,
			.scaler.outheight = SENSOR_HEIGHT_SECOND,

			.picWidth = SENSOR_WIDTH_SECOND,
			.picHeight = SENSOR_HEIGHT_SECOND,
		   },
		.framesource_chn =	{ DEV_ID_FS, 1, 0},
		.imp_encoder = { DEV_ID_ENC, 1, 0},
	},
};

IMPSensorInfo sensor_info;
int sample_system_init()
{
	int ret = 0;

	memset(&sensor_info, 0, sizeof(IMPSensorInfo));
	memcpy(sensor_info.name, SENSOR_NAME, sizeof(SENSOR_NAME));
	sensor_info.cbus_type = SENSOR_CUBS_TYPE;
	memcpy(sensor_info.i2c.type, SENSOR_NAME, sizeof(SENSOR_NAME));
	sensor_info.i2c.addr = SENSOR_I2C_ADDR;

	IMP_LOG_DBG(TAG, "sample_system_init start\n");

	ret = IMP_ISP_Open();
	if(ret < 0){
		IMP_LOG_ERR(TAG, "failed to open ISP\n");
		return -1;
	}

	ret = IMP_ISP_AddSensor(&sensor_info);
	if(ret < 0){
		IMP_LOG_ERR(TAG, "failed to AddSensor\n");
		return -1;
	}

	ret = IMP_ISP_EnableSensor();
	if(ret < 0){
		IMP_LOG_ERR(TAG, "failed to EnableSensor\n");
		return -1;
	}

	ret = IMP_System_Init();
	if(ret < 0){
		IMP_LOG_ERR(TAG, "IMP_System_Init failed\n");
		return -1;
	}

	/* enable turning, to debug graphics */
	ret = IMP_ISP_EnableTuning();
	if(ret < 0){
		IMP_LOG_ERR(TAG, "IMP_ISP_EnableTuning failed\n");
		return -1;
	}

    ret = IMP_ISP_Tuning_SetSensorFPS(SENSOR_FRAME_RATE_NUM, SENSOR_FRAME_RATE_DEN);
    if (ret < 0){
        IMP_LOG_ERR(TAG, "failed to set sensor fps\n");
        return -1;
    }

	IMP_LOG_INFO(TAG, "ImpSystemInit success\n");

	return 0;
}

int sample_system_exit()
{
	int ret = 0;

	IMP_LOG_DBG(TAG, "sample_system_exit start\n");


	IMP_System_Exit();

	ret = IMP_ISP_DisableSensor();
	if(ret < 0){
		IMP_LOG_ERR(TAG, "failed to DisableSensor\n");
		return -1;
	}

	ret = IMP_ISP_DelSensor(&sensor_info);
	if(ret < 0){
		IMP_LOG_ERR(TAG, "failed to DelSensor\n");
		return -1;
	}

	ret = IMP_ISP_DisableTuning();
	if(ret < 0){
		IMP_LOG_ERR(TAG, "IMP_ISP_DisableTuning failed\n");
		return -1;
	}

	if(IMP_ISP_Close()){
		IMP_LOG_ERR(TAG, "failed to close ISP\n");
		return -1;
	}

	IMP_LOG_DBG(TAG, " sample_system_exit success\n");

	return 0;
}

int sample_framesource_streamon()
{
	int ret = 0, i = 0;
	/* Enable channels */
	for (i = 0; i < FS_CHN_NUM; i++) {
		if (chn[i].enable) {
			ret = IMP_FrameSource_EnableChn(chn[i].index);
			if (ret < 0) {
				IMP_LOG_ERR(TAG, "IMP_FrameSource_EnableChn(%d) error: %d\n", ret, chn[i].index);
				return -1;
			}
		}
	}
	return 0;
}

int sample_framesource_streamoff()
{
	int ret = 0, i = 0;
	/* Enable channels */
	for (i = 0; i < FS_CHN_NUM; i++) {
		if (chn[i].enable){
			ret = IMP_FrameSource_DisableChn(chn[i].index);
			if (ret < 0) {
				IMP_LOG_ERR(TAG, "IMP_FrameSource_DisableChn(%d) error: %d\n", ret, chn[i].index);
				return -1;
			}
		}
	}
	return 0;
}

int sample_framesource_init()
{
	int i, ret;

	for (i = 0; i <  FS_CHN_NUM; i++) {
		if (chn[i].enable) {
			ret = IMP_FrameSource_CreateChn(chn[i].index, &chn[i].fs_chn_attr);
			if(ret < 0){
				IMP_LOG_ERR(TAG, "IMP_FrameSource_CreateChn(chn%d) error !\n", chn[i].index);
				return -1;
			}

			ret = IMP_FrameSource_SetChnAttr(chn[i].index, &chn[i].fs_chn_attr);
			if (ret < 0) {
				IMP_LOG_ERR(TAG, "IMP_FrameSource_SetChnAttr(chn%d) error !\n",  chn[i].index);
				return -1;
			}
		}
	}

	return 0;
}

int sample_framesource_exit()
{
	int ret,i;

	for (i = 0; i <  FS_CHN_NUM; i++) {
		if (chn[i].enable) {
			/*Destroy channel i*/
			ret = IMP_FrameSource_DestroyChn(i);
			if (ret < 0) {
				IMP_LOG_ERR(TAG, "IMP_FrameSource_DestroyChn() error: %d\n", ret);
				return -1;
			}
		}
	}
	return 0;
}

int sample_jpeg_init()
{
	int i, ret;
	IMPEncoderAttr *enc_attr;
	IMPEncoderCHNAttr channel_attr;
	IMPFSChnAttr *imp_chn_attr_tmp;

	for (i = 0; i <  FS_CHN_NUM; i++) {
		if (chn[i].enable) {
			imp_chn_attr_tmp = &chn[i].fs_chn_attr;
			memset(&channel_attr, 0, sizeof(IMPEncoderCHNAttr));
			enc_attr = &channel_attr.encAttr;
			enc_attr->enType = PT_JPEG;
			enc_attr->bufSize = 0;
			enc_attr->profile = 0;
			enc_attr->picWidth = imp_chn_attr_tmp->picWidth;
			enc_attr->picHeight = imp_chn_attr_tmp->picHeight;

			/* Create Channel */
			ret = IMP_Encoder_CreateChn(2 + chn[i].index, &channel_attr);
			if (ret < 0) {
				IMP_LOG_ERR(TAG, "IMP_Encoder_CreateChn(%d) error: %d\n",
							chn[i].index, ret);
				return -1;
			}

			/* Resigter Channel */
			ret = IMP_Encoder_RegisterChn(i, 2 + chn[i].index);
			if (ret < 0) {
				IMP_LOG_ERR(TAG, "IMP_Encoder_RegisterChn(0, %d) error: %d\n",
							chn[i].index, ret);
				return -1;
			}
		}
	}

	return 0;
}

//Config File Stuff

static int handler(void* user, const char* section, const char* name, const char* value){
	app_config_t* pconfig = (app_config_t*)user;
	#define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0
	if (MATCH("user", "ENCODING_TYPE")){
		pconfig->ENCODING_TYPE = atoi(value);
	} else if (MATCH("user", "MAXQP")){
		pconfig->MAXQP  = atoi(value);
	} else if (MATCH("user", "MINQP")){
		pconfig->MINQP  = atoi(value);
	} else if (MATCH("user", "BIASLVL")){
		pconfig->BIASLVL  = atoi(value);
	} else if (MATCH("user", "FROMQPSTEP")){
		pconfig->FROMQPSTEP  = atoi(value);
	} else if (MATCH("user", "GOPQPSTEP")){
		pconfig->GOPQPSTEP  = atoi(value);
	} else if (MATCH("user", "WIDTH")){
      	pconfig->WIDTH  = atoi(value);
    } else if (MATCH("user", "HEIGHT")){
        pconfig->HEIGHT  = atoi(value);
    } else if (MATCH("user", "RATENUM")){
        pconfig->RATENUM  = atoi(value);
    } else if (MATCH("user", "RATEDEN")){
        pconfig->RATEDEN  = atoi(value);
    } else if (MATCH("user", "PROFILE")){
        pconfig->PROFILE  = atoi(value);
    } else if (MATCH("user", "BITRATE")){
		pconfig->BITRATE  = atof(value);
	} else if (MATCH("recording", "ENABLED")){
		pconfig->recording_enabled = atoi(value);
	} else if (MATCH("recording", "OUTPUT_DIR")){
		strncpy(pconfig->recording_output_dir, value, sizeof(pconfig->recording_output_dir) - 1);
		pconfig->recording_output_dir[sizeof(pconfig->recording_output_dir) - 1] = '\0';
	} else if (MATCH("recording", "CHUNK_DURATION")){
		pconfig->recording_chunk_duration = atoi(value);
	} else if (MATCH("recording", "DISK_USAGE_THRESHOLD")){
		pconfig->recording_disk_threshold = atoi(value);
	} else if (MATCH("grafana", "GRAFANA_ENABLED")){
		if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0)
			pconfig->grafana_enabled = 1;
		else
			pconfig->grafana_enabled = 0;
	} else if (MATCH("grafana", "GRAFANA_PUSH_URL")){
		strncpy(pconfig->grafana_push_url, value, sizeof(pconfig->grafana_push_url) - 1);
		pconfig->grafana_push_url[sizeof(pconfig->grafana_push_url) - 1] = '\0';
	} else if (MATCH("grafana", "GRAFANA_USERNAME")){
		strncpy(pconfig->grafana_username, value, sizeof(pconfig->grafana_username) - 1);
		pconfig->grafana_username[sizeof(pconfig->grafana_username) - 1] = '\0';
	} else if (MATCH("grafana", "GRAFANA_API_KEY")){
		strncpy(pconfig->grafana_api_key, value, sizeof(pconfig->grafana_api_key) - 1);
		pconfig->grafana_api_key[sizeof(pconfig->grafana_api_key) - 1] = '\0';
	} else if (MATCH("grafana", "GRAFANA_PUSH_INTERVAL_MS")){
		pconfig->grafana_push_interval_ms = atoi(value);
	} else if (MATCH("upload", "ENABLED")){
		pconfig->upload_enabled = atoi(value);
	} else if (MATCH("upload", "URL")){
		strncpy(pconfig->upload_url, value, sizeof(pconfig->upload_url) - 1);
		pconfig->upload_url[sizeof(pconfig->upload_url) - 1] = '\0';
	} else if (MATCH("upload", "TOKEN")){
		strncpy(pconfig->upload_token, value, sizeof(pconfig->upload_token) - 1);
		pconfig->upload_token[sizeof(pconfig->upload_token) - 1] = '\0';
	} else if (MATCH("upload", "RATE_LIMIT_KBPS")){
		pconfig->upload_rate_limit_kbps = atoi(value);
	} else if (MATCH("upload", "SCAN_INTERVAL_S")){
		pconfig->upload_scan_interval_s = atoi(value);
	} else if (MATCH("upload", "BUFFER_IN_MEMORY")){
		pconfig->upload_buffer_in_memory = atoi(value);
	} else if (MATCH("upload", "ADAPTIVE_RATE")){
		pconfig->upload_adaptive_rate = atoi(value);
	} else if (MATCH("audio", "ENABLED")){
		pconfig->audio_enabled = atoi(value);
	} else if (MATCH("audio", "DEV_ID")){
		pconfig->audio_dev_id = atoi(value);
	} else if (MATCH("audio", "SAMPLE_RATE")){
		pconfig->audio_sample_rate = atoi(value);
	} else if (MATCH("http", "ENABLED")){
		pconfig->http_enabled = atoi(value);
	} else if (MATCH("http", "PORT")){
		pconfig->http_port = atoi(value);
	} else if (MATCH("smart", "GOP_SEC")){
		pconfig->SMART_GOP_SEC = atoi(value);
	} else if (MATCH("smart", "MAXQP")){
		pconfig->SMART_MAXQP = atoi(value);
	} else if (MATCH("smart", "MINQP")){
		pconfig->SMART_MINQP = atoi(value);
	} else if (MATCH("smart", "STATIC_TIME")){
		pconfig->SMART_STATIC_TIME = atoi(value);
	} else if (MATCH("smart", "QUALITY_LVL")){
		pconfig->SMART_QUALITY_LVL = atoi(value);
	} else if (MATCH("smart", "MAX_BITRATE")){
		pconfig->SMART_MAX_BITRATE = atoi(value);
	} else if (MATCH("smart", "FRM_QPSTEP")){
		pconfig->SMART_FRM_QPSTEP = atoi(value);
	} else if (MATCH("smart", "GOP_QPSTEP")){
		pconfig->SMART_GOP_QPSTEP = atoi(value);
	} else if (MATCH("smart", "CHANGE_POS")){
		pconfig->SMART_CHANGE_POS = atoi(value);
	} else if (MATCH("night", "FPS_NUM")){
		pconfig->NIGHT_FPS_NUM = atoi(value);
	} else if (MATCH("night", "FPS_DEN")){
		pconfig->NIGHT_FPS_DEN = atoi(value);
	} else if (MATCH("night", "BITRATE")){
		pconfig->NIGHT_BITRATE = atoi(value);
	} else if (MATCH("night", "MAXQP")){
		pconfig->NIGHT_MAXQP = atoi(value);
	} else if (MATCH("night", "QUALITY_LVL")){
		pconfig->NIGHT_QUALITY_LVL = atoi(value);
	} else if (MATCH("autonight", "ENABLED")){
		pconfig->AUTONIGHT_ENABLED = atoi(value);
	} else if (MATCH("autonight", "NIGHT_THRESH")){
		pconfig->AUTONIGHT_NIGHT_THRESH = atoi(value);
	} else if (MATCH("autonight", "DAY_THRESH")){
		pconfig->AUTONIGHT_DAY_THRESH = atoi(value);
	} else if (MATCH("autonight", "IR_LED_THRESH")){
		pconfig->AUTONIGHT_IR_LED_THRESH = atoi(value);
	} else if (MATCH("autonight", "IR_LED_OFF")){
		pconfig->AUTONIGHT_IR_LED_OFF = atoi(value);
	} else if (MATCH("autonight", "INTERVAL")){
		pconfig->AUTONIGHT_INTERVAL = atoi(value);
	} else {
		/* Unknown key/section: ignore rather than signal an error. minIni
		 * treats a 0 return as a parse error (it stops/aborts and returns
		 * the line number); returning 1 keeps the [telemetry] section —
		 * which this daemon doesn't consume — from poisoning the parse. */
		return 1;
	}
	return 1;
}

int app_config_parse(const char *ini_path, app_config_t *config)
{
	/* Set defaults */
	memset(config, 0, sizeof(app_config_t));
	config->recording_enabled = 1;
	strncpy(config->recording_output_dir, "/system/sdcard/DCIM/Recording",
			sizeof(config->recording_output_dir) - 1);
	config->recording_chunk_duration = 300;
	config->recording_disk_threshold = 90;
	config->grafana_enabled = 0;
	config->grafana_push_interval_ms = 60000;
	config->upload_enabled = 0;
	config->upload_rate_limit_kbps = 20;
	config->upload_scan_interval_s = 60;
	config->upload_buffer_in_memory = 0;
	config->upload_adaptive_rate = 0;
	config->audio_enabled = 0;        /* opt-in */
	config->audio_dev_id = 1;         /* analog mic */
	config->audio_sample_rate = 8000;
	config->http_enabled = 0;
	config->http_port = 8080;
	/* Smart mode defaults — optimized for indoor cameras */
	config->SMART_GOP_SEC = 2;
	config->SMART_MAXQP = 40;
	config->SMART_MINQP = 0;
	config->SMART_STATIC_TIME = 2;
	config->SMART_QUALITY_LVL = 3;
	config->SMART_MAX_BITRATE = 5000;
	config->SMART_FRM_QPSTEP = 3;
	config->SMART_GOP_QPSTEP = 15;
	config->SMART_CHANGE_POS = 80;
	/* Night mode: 0 = disabled */
	config->NIGHT_FPS_NUM = 0;
	config->NIGHT_FPS_DEN = 0;
	config->NIGHT_BITRATE = 0;
	config->NIGHT_MAXQP = 0;
	config->NIGHT_QUALITY_LVL = 0;
	/* Autonight: built-in photosensitive detection */
	config->AUTONIGHT_ENABLED = 0;
	config->AUTONIGHT_NIGHT_THRESH = 1200000;
	config->AUTONIGHT_DAY_THRESH = 930000;
	config->AUTONIGHT_IR_LED_THRESH = 3000000;
	config->AUTONIGHT_IR_LED_OFF = 0;
	config->AUTONIGHT_INTERVAL = 3;

	if (ini_parse(ini_path, handler, config) < 0) {
		printf("[config] Can't load %s\n", ini_path);
		return -1;
	}

	printf("[config] Loaded %s\n", ini_path);
	printf("[config] Encoding: type=%d qp=%d-%d bitrate=%.0f profile=%d\n",
			config->ENCODING_TYPE, config->MINQP, config->MAXQP,
			config->BITRATE, config->PROFILE);
	printf("[config] Resolution: %dx%d @ %d/%d fps\n",
			config->WIDTH, config->HEIGHT, config->RATENUM, config->RATEDEN);
	printf("[config] Recording: enabled=%d dir=%s chunk=%ds threshold=%d%%\n",
			config->recording_enabled, config->recording_output_dir,
			config->recording_chunk_duration, config->recording_disk_threshold);
	printf("[config] Grafana: enabled=%d interval=%dms url=%s\n",
			config->grafana_enabled, config->grafana_push_interval_ms,
			config->grafana_push_url);
	printf("[config] Upload: enabled=%d rate=%s%dKB/s scan=%ds buffer=%d url=%s\n",
			config->upload_enabled,
			config->upload_adaptive_rate ? "adaptive,cap=" : "",
			config->upload_rate_limit_kbps,
			config->upload_scan_interval_s, config->upload_buffer_in_memory,
			config->upload_url);
	printf("[config] Audio: enabled=%d dev_id=%d sample_rate=%d\n",
			config->audio_enabled, config->audio_dev_id, config->audio_sample_rate);
	printf("[config] HTTP: enabled=%d port=%d\n",
			config->http_enabled, config->http_port);
	printf("[config] Smart GOP: %ds (maxGop will scale with fps)\n",
			config->SMART_GOP_SEC);

	return 0;
}

/* Global config pointer set by main before calling sample_encoder_init */
static app_config_t *g_app_config = NULL;

void sample_encoder_set_config(app_config_t *config)
{
	g_app_config = config;
}

int sample_encoder_init()
{
	app_config_t *pconfig = g_app_config;
	if (!pconfig) {
		printf("No config set, call sample_encoder_set_config first\n");
		return -1;
	}
	app_config_t config = *pconfig;

	printf("Config loaded\n");
	printf("ENCODING TYPE: %d \n", config.ENCODING_TYPE);

	int S_RC_METHOD = config.ENCODING_TYPE;
	int maxqp = config.MAXQP;
	int minqp = config.MINQP;
	int biaslvl = config.BIASLVL;
	int fromqpstep = config.FROMQPSTEP;
	int gopqpstep = config.GOPQPSTEP;
	double bitrate = config.BITRATE;
	int width = config.WIDTH;
	int height = config.HEIGHT;
    int rateNum = config.RATENUM;
    int rateDen = config.RATEDEN;
    int profile = config.PROFILE;

	int i, ret;
	IMPEncoderAttr *enc_attr;
	IMPEncoderRcAttr *rc_attr;
	IMPFSChnAttr *imp_chn_attr_tmp;
	IMPEncoderCHNAttr channel_attr;

	for (i = 0; i <  FS_CHN_NUM; i++) {
		if (chn[i].enable) {
			imp_chn_attr_tmp = &chn[i].fs_chn_attr;
			printf("\t - channel %d enabled\n", i);
			printf("\t\tdefault width %d height %d\n", imp_chn_attr_tmp->picWidth, imp_chn_attr_tmp->picHeight);
			if (width != 0 && height != 0){
			    imp_chn_attr_tmp->picWidth = width;
			    imp_chn_attr_tmp->picHeight = height;
			    printf("\t\toverride width %d height %d\n", width, height);
			}
			printf("\t\tdefault rate %d/%d\n", imp_chn_attr_tmp->outFrmRateNum, imp_chn_attr_tmp->outFrmRateDen);
            if (rateDen != 0 && rateNum != 0){
                imp_chn_attr_tmp->outFrmRateNum = rateNum;
                imp_chn_attr_tmp->outFrmRateDen = rateDen;
                printf("\t\toverride rate %d/%d\n", rateNum, rateDen);
            }
			memset(&channel_attr, 0, sizeof(IMPEncoderCHNAttr));
			enc_attr = &channel_attr.encAttr;
			enc_attr->enType = PT_H264;
			enc_attr->bufSize = 0;
			enc_attr->profile = profile;
			enc_attr->picWidth = imp_chn_attr_tmp->picWidth;
			enc_attr->picHeight = imp_chn_attr_tmp->picHeight;
			rc_attr = &channel_attr.rcAttr;
            rc_attr->outFrmRate.frmRateNum = imp_chn_attr_tmp->outFrmRateNum;
            rc_attr->outFrmRate.frmRateDen = imp_chn_attr_tmp->outFrmRateDen;
            {
                /* GOP = I-frame interval. Longer GOPs cut I-frame cost and
                 * stretch the smart-skip cycle on static scenes (1 encoded
                 * frame per GOP); 2s is the conservative legacy default. */
                int gop_sec = config.SMART_GOP_SEC;
                if (gop_sec < 1) gop_sec = 2;
                if (gop_sec > 10) gop_sec = 10;
                rc_attr->maxGop = gop_sec * rc_attr->outFrmRate.frmRateNum
						/ rc_attr->outFrmRate.frmRateDen;
            }
            if (S_RC_METHOD == ENC_RC_MODE_CBR) {
				printf("CBR MODE SELECTED \n");
                rc_attr->attrRcMode.rcMode = ENC_RC_MODE_CBR;
                rc_attr->attrRcMode.attrH264Cbr.outBitRate = (double)bitrate * (imp_chn_attr_tmp->picWidth * imp_chn_attr_tmp->picHeight) / (1920 * 1080);
                rc_attr->attrRcMode.attrH264Cbr.maxQp = maxqp;
                rc_attr->attrRcMode.attrH264Cbr.minQp = minqp;
                rc_attr->attrRcMode.attrH264Cbr.iBiasLvl = biaslvl;
                rc_attr->attrRcMode.attrH264Cbr.frmQPStep = fromqpstep;
                rc_attr->attrRcMode.attrH264Cbr.gopQPStep = gopqpstep;
                rc_attr->attrRcMode.attrH264Cbr.adaptiveMode = false;
                rc_attr->attrRcMode.attrH264Cbr.gopRelation = false;

                rc_attr->attrHSkip.hSkipAttr.skipType = IMP_Encoder_STYPE_N1X;
                rc_attr->attrHSkip.hSkipAttr.m = 0;
                rc_attr->attrHSkip.hSkipAttr.n = 0;
                rc_attr->attrHSkip.hSkipAttr.maxSameSceneCnt = 0;
                rc_attr->attrHSkip.hSkipAttr.bEnableScenecut = 0;
                rc_attr->attrHSkip.hSkipAttr.bBlackEnhance = 0;
                rc_attr->attrHSkip.maxHSkipType = IMP_Encoder_STYPE_N1X;
            } else if (S_RC_METHOD == ENC_RC_MODE_VBR) {
				printf("VBR MODE SELECTED \n");
                rc_attr->attrRcMode.rcMode = ENC_RC_MODE_VBR;
                rc_attr->attrRcMode.attrH264Vbr.maxQp = 45;
                rc_attr->attrRcMode.attrH264Vbr.minQp = 15;
                rc_attr->attrRcMode.attrH264Vbr.staticTime = 2;
                rc_attr->attrRcMode.attrH264Vbr.maxBitRate = (double)2000.0 * (imp_chn_attr_tmp->picWidth * imp_chn_attr_tmp->picHeight) / (1280 * 720);
                rc_attr->attrRcMode.attrH264Vbr.iBiasLvl = 0;
                rc_attr->attrRcMode.attrH264Vbr.changePos = 80;
                rc_attr->attrRcMode.attrH264Vbr.qualityLvl = 2;
                rc_attr->attrRcMode.attrH264Vbr.frmQPStep = 3;
                rc_attr->attrRcMode.attrH264Vbr.gopQPStep = 15;
                rc_attr->attrRcMode.attrH264Vbr.gopRelation = false;

                rc_attr->attrHSkip.hSkipAttr.skipType = IMP_Encoder_STYPE_N1X;
                rc_attr->attrHSkip.hSkipAttr.m = 0;
                rc_attr->attrHSkip.hSkipAttr.n = 0;
                rc_attr->attrHSkip.hSkipAttr.maxSameSceneCnt = 0;
                rc_attr->attrHSkip.hSkipAttr.bEnableScenecut = 0;
                rc_attr->attrHSkip.hSkipAttr.bBlackEnhance = 0;
                rc_attr->attrHSkip.maxHSkipType = IMP_Encoder_STYPE_N1X;
            } else if (S_RC_METHOD == ENC_RC_MODE_SMART) {
				printf("SMART MODE SELECTED \n");
                rc_attr->attrRcMode.rcMode = ENC_RC_MODE_SMART;
                rc_attr->attrRcMode.attrH264Smart.maxQp = config.SMART_MAXQP;
                rc_attr->attrRcMode.attrH264Smart.minQp = config.SMART_MINQP;
                rc_attr->attrRcMode.attrH264Smart.staticTime = config.SMART_STATIC_TIME;
                rc_attr->attrRcMode.attrH264Smart.maxBitRate = (double)config.SMART_MAX_BITRATE * (imp_chn_attr_tmp->picWidth * imp_chn_attr_tmp->picHeight) / (1920 * 1080);
                rc_attr->attrRcMode.attrH264Smart.iBiasLvl = 0;
                rc_attr->attrRcMode.attrH264Smart.changePos = config.SMART_CHANGE_POS;
                rc_attr->attrRcMode.attrH264Smart.qualityLvl = config.SMART_QUALITY_LVL;
                rc_attr->attrRcMode.attrH264Smart.frmQPStep = config.SMART_FRM_QPSTEP;
                rc_attr->attrRcMode.attrH264Smart.gopQPStep = config.SMART_GOP_QPSTEP;
                rc_attr->attrRcMode.attrH264Smart.gopRelation = false;

                rc_attr->attrHSkip.hSkipAttr.skipType = IMP_Encoder_STYPE_N1X;
                rc_attr->attrHSkip.hSkipAttr.m = rc_attr->maxGop - 1;
                rc_attr->attrHSkip.hSkipAttr.n = 1;
                rc_attr->attrHSkip.hSkipAttr.maxSameSceneCnt = 6;
                rc_attr->attrHSkip.hSkipAttr.bEnableScenecut = 0;
                rc_attr->attrHSkip.hSkipAttr.bBlackEnhance = 0;
                rc_attr->attrHSkip.maxHSkipType = IMP_Encoder_STYPE_N1X;
            } else { /* fixQp */
				printf("Fixed QP MODE SELECTED \n");
                rc_attr->attrRcMode.rcMode = ENC_RC_MODE_FIXQP;
                rc_attr->attrRcMode.attrH264FixQp.qp = 42;

                rc_attr->attrHSkip.hSkipAttr.skipType = IMP_Encoder_STYPE_N1X;
                rc_attr->attrHSkip.hSkipAttr.m = 0;
                rc_attr->attrHSkip.hSkipAttr.n = 0;
                rc_attr->attrHSkip.hSkipAttr.maxSameSceneCnt = 0;
                rc_attr->attrHSkip.hSkipAttr.bEnableScenecut = 0;
                rc_attr->attrHSkip.hSkipAttr.bBlackEnhance = 0;
                rc_attr->attrHSkip.maxHSkipType = IMP_Encoder_STYPE_N1X;
            }

			ret = IMP_Encoder_CreateChn(chn[i].index, &channel_attr);
			if (ret < 0) {
				IMP_LOG_ERR(TAG, "IMP_Encoder_CreateChn(%d) error !\n", i);
				return -1;
			}

			ret = IMP_Encoder_RegisterChn(chn[i].index, chn[i].index);
			if (ret < 0) {
				IMP_LOG_ERR(TAG, "IMP_Encoder_RegisterChn(%d, %d) error: %d\n",
						chn[i].index, chn[i].index, ret);
				return -1;
			}
		}
	}
	printf("ENCODER IS RUNNING SUCCESSFULLY! \n");
	return 0;
}

static int encoder_chn_exit(int encChn)
{
	int ret;
	IMPEncoderCHNStat chn_stat;
	ret = IMP_Encoder_Query(encChn, &chn_stat);
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "IMP_Encoder_Query(%d) error: %d\n",
					encChn, ret);
		return -1;
	}

	if (chn_stat.registered) {
		ret = IMP_Encoder_UnRegisterChn(encChn);
		if (ret < 0) {
			IMP_LOG_ERR(TAG, "IMP_Encoder_UnRegisterChn(%d) error: %d\n",
						encChn, ret);
			return -1;
		}

		ret = IMP_Encoder_DestroyChn(encChn);
		if (ret < 0) {
			IMP_LOG_ERR(TAG, "IMP_Encoder_DestroyChn(%d) error: %d\n",
						encChn, ret);
			return -1;
		}
	}

	return 0;
}

int sample_encoder_exit(void)
{
	int ret;

	ret = encoder_chn_exit(ENC_H264_CHANNEL);
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "Encoder Channel %d exit  error: %d\n",
					ENC_H264_CHANNEL, ret);
		return -1;
	}

	ret = encoder_chn_exit(ENC_JPEG_CHANNEL);
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "Encoder Channel %d exit  error: %d\n",
					ENC_JPEG_CHANNEL, ret);
		return -1;
	}

	ret = IMP_Encoder_DestroyGroup(0);
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "IMP_Encoder_DestroyGroup(0) error: %d\n", ret);
		return -1;
	}

	return 0;
}

static int save_stream(int fd, IMPEncoderStream *stream)
{
	int ret, i, nr_pack = stream->packCount;

	for (i = 0; i < nr_pack; i++) {
		ret = write(fd, (void *)stream->pack[i].virAddr,
					stream->pack[i].length);
		if (ret != stream->pack[i].length) {
			IMP_LOG_ERR(TAG, "stream write error:%s\n", strerror(errno));
			return -1;
		}
		//IMP_LOG_DBG(TAG, "stream->pack[%d].dataType=%d\n", i, ((int)stream->pack[i].dataType.h264Type));
	}

	return 0;
}

int sample_do_get_jpeg_snap(void)
{
	int ret;

	/* JEPG Channel start receive picture */
	ret = IMP_Encoder_StartRecvPic(ENC_JPEG_CHANNEL);
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "IMP_Encoder_StartRecvPic(%d) failed\n", ENC_JPEG_CHANNEL);
		return -1;
	}

	time_t now;
	time(&now);
	struct tm *now_tm;
	now_tm = localtime(&now);
	char now_str[32];
	strftime(now_str, sizeof(now_str), "%Y%m%d%H%M%S", now_tm);

	char snap_path[128];
	sprintf(snap_path, "%s/snap-%s.jpg",
			SNAP_FILE_PATH_PREFIX, now_str);

	IMP_LOG_ERR(TAG, "Open Snap file %s ", snap_path);
	int snap_fd = open(snap_path, O_RDWR | O_CREAT | O_TRUNC, 0777);
	if (snap_fd < 0) {
		IMP_LOG_ERR(TAG, "failed: %s\n", strerror(errno));
		ret = -1;
		goto stop_recv;
	}
	IMP_LOG_DBG(TAG, "OK\n");

	IMPEncoderStream stream;
	int have_stream = 0;

	/* Polling JPEG Snap, set timeout as 1000msec */
	ret = IMP_Encoder_PollingStream(ENC_JPEG_CHANNEL, 1000);
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "Polling stream timeout\n");
		ret = -1;
		goto cleanup;
	}

	/* Get JPEG Snap */
	ret = IMP_Encoder_GetStream(ENC_JPEG_CHANNEL, &stream, 1);
	if (ret < 0) {
		IMP_LOG_ERR(TAG, "IMP_Encoder_GetStream() failed\n");
		ret = -1;
		goto cleanup;
	}
	have_stream = 1;

	ret = save_stream(snap_fd, &stream);
	if (ret < 0)
		IMP_LOG_ERR(TAG, "save_stream() failed\n");

cleanup:
	if (have_stream)
		IMP_Encoder_ReleaseStream(ENC_JPEG_CHANNEL, &stream);
	if (snap_fd >= 0)
		close(snap_fd);

stop_recv:
	{
		/* Always balance StartRecvPic — leaving the channel in receive mode
		 * makes every later snapshot fail until the daemon restarts. */
		int sr = IMP_Encoder_StopRecvPic(ENC_JPEG_CHANNEL);
		if (sr < 0)
			IMP_LOG_ERR(TAG, "IMP_Encoder_StopRecvPic() failed\n");
	}

	return ret;
}

int sample_set_IRLED(int enable)
{
	int fd1;
	fd1 = open("/sys/class/gpio/gpio49/value", O_RDWR);
	if (fd1 < 0) {
		IMP_LOG_DBG(TAG, "open gpio 49 error !");
		return -1;
	}
	if (enable) {
		write(fd1, "0", 1);
	} else {
		write(fd1, "1", 1);
	}
	close(fd1);

	return 0;
}

int sample_set_IRCUT(int enable)
{
	int fd1, fd2;
	fd1 = open("/sys/class/gpio/gpio25/value", O_RDWR);
	if (fd1 < 0) {
		IMP_LOG_DBG(TAG, "open gpio 25 error !");
		return -1;
	}
	fd2 = open("/sys/class/gpio/gpio26/value", O_RDWR);
	if (fd2 < 0) {
		close(fd1);
		IMP_LOG_DBG(TAG, "open gpio 26 error !");
		return -1;
	}
	if (enable) {
		write(fd2, "1", 1);
	} else {
		write(fd1, "1", 1);
	}

	usleep(500*1000);
	write(fd1, "0", 1);
	write(fd2, "0", 1);

	close(fd1);
	close(fd2);

	return 0;
}

char *get_curr_timestr(char *buf) {
	time_t t;
	struct tm *tminfo;

	time(&t);
	tminfo = localtime(&t);
	sprintf(buf, "%02d:%02d:%02d", tminfo->tm_hour, tminfo->tm_min, tminfo->tm_sec);
	return buf;
}

static int  g_soft_ps_running = 1;

/* Defined in capture_and_encoding.cpp (C++) — linked at object level */
extern void apply_night_encoding(int night);

void *sample_soft_photosensitive_thread(void *p)
{
	app_config_t *cfg = (app_config_t *)p;
	int evDebugCount = 10000;
	int ev_err_count = 0;
	char tmstr[16];
	int avgExp = 0;
	int avgExp_init = 0;
	IMPISPRunningMode pmode;
	int ir_leds_active = 0;
	int last_night_state = -1;
	int isp_night = -1;	/* hysteresis-protected mode: 1=night 0=day, -1=unknown */

	int night_thresh = cfg->AUTONIGHT_NIGHT_THRESH;
	int day_thresh = cfg->AUTONIGHT_DAY_THRESH;
	int ir_led_thresh = cfg->AUTONIGHT_IR_LED_THRESH;
	int ir_led_off = cfg->AUTONIGHT_IR_LED_OFF;
	int interval = cfg->AUTONIGHT_INTERVAL;

	printf("[autonight] Starting: night_thresh=%d day_thresh=%d ir_led_thresh=%d interval=%ds ir_led_off=%d\n",
			night_thresh, day_thresh, ir_led_thresh, interval, ir_led_off);

	while (g_soft_ps_running) {
		IMPISPEVAttr expAttr;
		int ret = IMP_ISP_Tuning_GetEVAttr(&expAttr);
		if (ret != 0) {
			ev_err_count++;
			if (ev_err_count <= 3)
				printf("[autonight] GetEVAttr failed (%d), retrying...\n", ev_err_count);
			sleep(interval);
			continue;
		}
		if (ev_err_count > 0) {
			printf("[autonight] GetEVAttr recovered after %d failures\n", ev_err_count);
			ev_err_count = 0;
		}

		if (evDebugCount > 0) {
			printf("[autonight] EV: exp %d aGain %d dGain %d\n",
					expAttr.ev, expAttr.again, expAttr.dgain);
			evDebugCount--;
		}

		/* EMA with fixed alpha = 1/4 (shift-based, no division) */
		if (!avgExp_init) {
			avgExp = expAttr.ev;
			avgExp_init = 1;
		} else {
			avgExp = avgExp - (avgExp >> 2) + (expAttr.ev >> 2);
		}

		IMP_ISP_Tuning_GetISPRunningMode(&pmode);
		if (isp_night < 0)
			isp_night = (pmode == IMPISP_RUNNING_MODE_NIGHT) ? 1 : 0;

		/* Night/day ISP mode switching */
		if (avgExp > night_thresh) {
			if (pmode != IMPISP_RUNNING_MODE_NIGHT) {
				printf("[%s] avgExp %d > %d -> NIGHT\n",
						get_curr_timestr((char *) &tmstr), avgExp, night_thresh);
				evDebugCount = 10;
				IMP_ISP_Tuning_SetISPRunningMode(IMPISP_RUNNING_MODE_NIGHT);
				sample_set_IRCUT(1);
				isp_night = 1;
			}
		} else if (avgExp < day_thresh) {
			if (pmode != IMPISP_RUNNING_MODE_DAY) {
				printf("[%s] avgExp %d < %d -> DAY\n",
						get_curr_timestr((char *) &tmstr), avgExp, day_thresh);
				evDebugCount = 10;
				IMP_ISP_Tuning_SetISPRunningMode(IMPISP_RUNNING_MODE_DAY);
				sample_set_IRCUT(0);
				isp_night = 0;
			}
		}

		/* Encoding follows the hysteresis-protected ISP state. Driving it
		 * from the raw threshold flaps around night_thresh and reconfigures
		 * the encoder several times per minute at dusk/dawn. */
		{
			if (isp_night != last_night_state) {
				g_night_mode = isp_night;
				apply_night_encoding(isp_night);
				printf("[%s] Night mode %s (avgExp=%d)\n",
						get_curr_timestr((char *) &tmstr),
						isp_night ? "ON" : "OFF", avgExp);
				last_night_state = isp_night;
			}
		}

		/* IR LED control with hysteresis: turn on above ir_led_thresh, off
		 * only below 3/4 of it. A single threshold oscillates — the IR
		 * light itself lowers exposure below the threshold again. */
		{
			int want_on = ir_leds_active
				? (avgExp > ir_led_thresh * 3 / 4)
				: (avgExp > ir_led_thresh);
			if (ir_led_off) want_on = !want_on;

			if (want_on && !ir_leds_active) {
				printf("[%s] avgExp %d -> IR LEDs ON\n",
						get_curr_timestr((char *) &tmstr), avgExp);
				sample_set_IRLED(1);
				ir_leds_active = 1;
			} else if (!want_on && ir_leds_active) {
				printf("[%s] avgExp %d -> IR LEDs OFF\n",
						get_curr_timestr((char *) &tmstr), avgExp);
				sample_set_IRLED(0);
				ir_leds_active = 0;
			}
		}

		sleep(interval);
	}

	return NULL;
}

