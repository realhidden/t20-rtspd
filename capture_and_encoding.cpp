#include <string.h>    
#include <errno.h>      

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>

#include <imp/imp_isp.h>
#include <imp/imp_system.h>
#include <imp/imp_log.h>
#include <imp/imp_framesource.h>
#include <imp/imp_encoder.h>

#include "imp-common.h"
#include "capture_and_encoding.h"

#define TAG "capture_and_encoding"

extern struct chn_conf chn[];

extern "C" {
extern int IMP_Encoder_SetPoolSize(int newPoolSize0);
}

volatile int g_night_mode = 0;
static app_config_t *g_app_config = NULL;

static void sigusr1_handler(int sig) {
	(void)sig;
	g_night_mode = !g_night_mode;
}

void apply_night_encoding(int night) {
	if (!g_app_config) return;
	int encChn = ENC_H264_CHANNEL;

	IMPEncoderAttrRcMode rcMode;
	int ret = IMP_Encoder_GetChnAttrRcMode(encChn, &rcMode);
	if (ret != 0) { printf("[night] GetChnRcMode failed: %d\n", ret); return; }

	if (night && g_app_config->NIGHT_MAXQP > 0) {
		printf("[night] Switching to NIGHT encoding (maxQp=%d bitrate=%d)\n",
			g_app_config->NIGHT_MAXQP, g_app_config->NIGHT_BITRATE);
		rcMode.attrH264Smart.maxQp = g_app_config->NIGHT_MAXQP;
		if (g_app_config->NIGHT_BITRATE > 0)
			rcMode.attrH264Smart.maxBitRate = (double)g_app_config->NIGHT_BITRATE *
				(chn[0].fs_chn_attr.picWidth * chn[0].fs_chn_attr.picHeight) / (1920 * 1080);
		if (g_app_config->NIGHT_QUALITY_LVL > 0)
			rcMode.attrH264Smart.qualityLvl = g_app_config->NIGHT_QUALITY_LVL;
	} else {
		printf("[night] Switching to DAY encoding (maxQp=%d)\n", g_app_config->SMART_MAXQP);
		rcMode.attrH264Smart.maxQp = g_app_config->SMART_MAXQP;
		rcMode.attrH264Smart.maxBitRate = (double)g_app_config->SMART_MAX_BITRATE *
			(chn[0].fs_chn_attr.picWidth * chn[0].fs_chn_attr.picHeight) / (1920 * 1080);
		rcMode.attrH264Smart.qualityLvl = g_app_config->SMART_QUALITY_LVL;
	}

	ret = IMP_Encoder_SetChnAttrRcMode(encChn, &rcMode);
	if (ret != 0) printf("[night] SetChnRcMode failed: %d\n", ret);

	if (night && g_app_config->NIGHT_FPS_NUM > 0) {
		IMPEncoderFrmRate fps;
		fps.frmRateNum = g_app_config->NIGHT_FPS_NUM;
		fps.frmRateDen = g_app_config->NIGHT_FPS_DEN;
		ret = IMP_Encoder_SetChnFrmRate(encChn, &fps);
		if (ret != 0) printf("[night] SetChnFrmRate failed: %d\n", ret);
		else printf("[night] FPS set to %d/%d\n", fps.frmRateNum, fps.frmRateDen);
	} else if (!night && g_app_config->RATENUM > 0) {
		IMPEncoderFrmRate fps;
		fps.frmRateNum = g_app_config->RATENUM;
		fps.frmRateDen = g_app_config->RATEDEN;
		ret = IMP_Encoder_SetChnFrmRate(encChn, &fps);
		if (ret != 0) printf("[night] SetChnFrmRate restore failed: %d\n", ret);
		else printf("[night] FPS restored to %d/%d\n", fps.frmRateNum, fps.frmRateDen);
	}
}

int destory()
{
	int ret, i;

	printf("[capture] Teardown starting...\n");

	/* Step.a Stop receiving pictures before teardown */
	ret = IMP_Encoder_StopRecvPic(0);
	if (ret < 0) {
		printf("[capture] IMP_Encoder_StopRecvPic() failed: %d\n", ret);
		return -1;
	}

	/* Step.b Stream Off */
	ret = sample_framesource_streamoff();
	if (ret < 0) {
		printf("[capture] FrameSource StreamOff failed: %d\n", ret);
		return -1;
	}

	/* Step.c UnBind */
	for (i = 0; i < FS_CHN_NUM; i++) {
		if (chn[i].enable) {
			ret = IMP_System_UnBind(&chn[i].framesource_chn, &chn[i].imp_encoder);
			if (ret < 0) {
				printf("[capture] UnBind channel%d failed: %d\n", i, ret);
				return -1;
			}
		}
	}

	/* Step.d Encoder exit */
	ret = sample_encoder_exit();
	if (ret < 0) {
		printf("[capture] Encoder exit failed: %d\n", ret);
		return -1;
	}

	/* Step.e FrameSource exit */
	ret = sample_framesource_exit();
	if (ret < 0) {
		printf("[capture] FrameSource exit failed: %d\n", ret);
		return -1;
	}

	/* Step.f System exit */
	ret = sample_system_exit();
	if (ret < 0) {
		printf("[capture] sample_system_exit() failed: %d\n", ret);
		return -1;
	}

	printf("[capture] Teardown complete\n");
	return 0;
}

int start_encoder_receiving(int chn)
{
	int ret = IMP_Encoder_StartRecvPic(chn);
	if (ret < 0) {
		printf("IMP_Encoder_StartRecvPic(%d) failed\n", chn);
		return -1;
	}
	return 0;
}

int capture_and_encoding(void *cfg)
{
	int ret = 0;
	int i = 0;
	app_config_t *config = (app_config_t *)cfg;

	/* Store config for runtime night mode switching */
	g_app_config = config;

	/* Register SIGUSR1 for night mode toggle */
	struct sigaction sa;
	sa.sa_handler = sigusr1_handler;
	sa.sa_flags = SA_RESTART;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGUSR1, &sa, NULL);
	printf("[capture] SIGUSR1 handler registered for night mode toggle\n");

	
	printf("[capture] Initializing pipeline...\n");

	// undocumented function to increase pool size
	IMP_Encoder_SetPoolSize(0x100000);

	ret = sample_system_init();
	if (ret < 0) {
		printf("[capture] IMP_System_Init() failed\n");
		return -1;
	}
	printf("[capture] IMP system initialized\n");

	/* Step.2 FrameSource init */
	ret = sample_framesource_init();
	if (ret < 0) {
		printf("[capture] FrameSource init failed\n");
		return -1;
	}
	printf("[capture] FrameSource initialized\n");

	for (i = 0; i < FS_CHN_NUM; i++) {
		if (chn[i].enable) {
			ret = IMP_Encoder_CreateGroup(chn[i].index);
			if (ret < 0) {
				printf("IMP_Encoder_CreateGroup(%d) error !\n", i);
				return -1;
			}
		}
	}

	/* Step.3 Encoder init */
	ret = sample_encoder_init();
	if (ret < 0) {
		printf("[capture] Encoder init failed\n");
		return -1;
	}
	printf("[capture] Encoder initialized\n");

	/* Step.4 Bind framesource channels to encoders */
	for (i = 0; i < FS_CHN_NUM; i++) {
		if (chn[i].enable) {
			ret = IMP_System_Bind(&chn[0].framesource_chn, &chn[i].imp_encoder);
			if (ret < 0) {
				printf("Bind FrameSource channel0 and Encoder failed\n");
				return -1;
			}
		}
	}

	/* Step.6 Stream On */
	ret = sample_framesource_streamon();
	if (ret < 0) {
		printf("[capture] FrameSource stream-on failed\n");
		return -1;
	}
	printf("[capture] FrameSource streaming\n");


	// start thread for autonight detection if enabled
	if (config->AUTONIGHT_ENABLED) {
		printf("[capture] Autonight thread starting\n");
		pthread_t autonight_tid;
		pthread_create(&autonight_tid, NULL, sample_soft_photosensitive_thread, config);
	} else {
		printf("[capture] Autonight disabled (set [autonight] ENABLED=1 in config)\n");
	}

	printf("[capture] Pipeline ready\n");
	return 0;
}
