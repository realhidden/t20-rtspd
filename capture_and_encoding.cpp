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

/* ISP denoise, saved at init so daytime can be restored exactly.
 *
 * The encoder thread spends ~24% of the core spinning while the hardware
 * encodes, and that scales with image complexity. At night there is no
 * working IR illuminator, so the AE runs at 103-128 dB analog gain and hands
 * the encoder amplified noise. Denoising in the ISP is upstream of the
 * encoder and is the only place reachable from userspace that can reduce
 * hardware encode time, output bytes and spin together.
 *
 * Keep the ISP's own daylight values so switching back does not leave the
 * camera looking denoised at noon.
 */
static IMPISPSinterDenoiseAttr g_sinter_saved;
static IMPISPTemperDenoiseAttr g_temper_saved;
static int g_denoise_saved;

static void isp_denoise_save(void)
{
	if (g_denoise_saved) return;
	if (IMP_ISP_Tuning_GetSinterDnsAttr(&g_sinter_saved) == 0 &&
	    IMP_ISP_Tuning_GetTemperDnsAttr(&g_temper_saved) == 0) {
		g_denoise_saved = 1;
		printf("[denoise] saved ISP sinter(enable=%d type=%d str=%d) "
				"temper(type=%d str=%d)\n",
				g_sinter_saved.enable, g_sinter_saved.type,
				g_sinter_saved.sinter_strength,
				g_temper_saved.type, g_temper_saved.temper_strength);
	} else {
		printf("[denoise] could not read current ISP denoise attrs\n");
	}
}

static void isp_denoise_apply(int night)
{
	if (!g_app_config) return;
	int strength = night ? g_app_config->NIGHT_ISP_DENOISE : 0;

	if (strength <= 0) {
		if (g_denoise_saved) {
			IMP_ISP_Tuning_SetSinterDnsAttr(&g_sinter_saved);
			IMP_ISP_Tuning_SetTemperDnsAttr(&g_temper_saved);
			printf("[denoise] restored ISP defaults (day)\n");
		}
		return;
	}

	isp_denoise_save();
	if (strength > 255) strength = 255;

	IMPISPSinterDenoiseAttr s = g_sinter_saved;
	s.enable = IMPISP_TUNING_OPS_MODE_ENABLE;
	s.type = IMPISP_TUNING_OPS_TYPE_MANUAL;
	s.sinter_strength = (unsigned char)strength;
	s.sval_min = (unsigned char)strength;
	s.sval_max = (unsigned char)strength;
	int r1 = IMP_ISP_Tuning_SetSinterDnsAttr(&s);

	IMPISPTemperDenoiseAttr t = g_temper_saved;
	t.type = IMPISP_TEMPER_MANUAL;
	t.temper_strength = (unsigned char)strength;
	t.tval_min = (unsigned char)strength;
	t.tval_max = (unsigned char)strength;
	int r2 = IMP_ISP_Tuning_SetTemperDnsAttr(&t);

	printf("[denoise] night strength=%d (sinter rc=%d, temper rc=%d)\n",
			strength, r1, r2);
}

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

	/* Color2Grey: drop the chroma planes and emit monochrome.
	 *
	 * This camera has no working IR illuminator (there is no /dev/pwm in this
	 * firmware, so the LED array the stock driver used to drive cannot be
	 * driven at all). At night the IR-cut opens onto nothing, the AE sits at
	 * 103-128 dB analog gain, and the sensor output is amplified noise with
	 * no colour information in it. Encoding chroma for that image is pure
	 * waste, and noise in the luma path is exactly what defeats the bitrate
	 * cap once QP saturates. So switch to monochrome at night and back to
	 * colour in daylight, where colour is real and worth paying for. */
	/* ISP denoise follows the same day/night switch. */
	isp_denoise_apply(night);

	{
		IMPEncoderColor2GreyCfg grey;
		grey.enable = night ? (g_app_config->NIGHT_COLOR2GREY ? 1 : 0)
		                    : (g_app_config->DAY_COLOR2GREY ? 1 : 0);
		ret = IMP_Encoder_SetChnColor2Grey(encChn, &grey);
		if (ret != 0)
			printf("[night] SetChnColor2Grey(%d) failed: %d\n", grey.enable, ret);
		else
			printf("[night] Color2Grey = %d\n", grey.enable);
	}

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

	/* Capture the ISP's own denoise settings so daytime can be restored
	 * after a night applies the configured strength. */
	isp_denoise_save();

	printf("[capture] Pipeline ready\n");
	return 0;
}
