/*
 * audio_capture.cpp — microphone capture (IMP audio-in) → G.711A (IMP AENC)
 * → MKV recorder.
 *
 * Runs a dedicated thread at the audio frame cadence, independent of the video
 * main loop. Encoded G.711A frames are passed to mkv_recorder_write_audio_frame()
 * with the IMP frame timestamp so the recorder can align A/V.
 */

#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include <imp/imp_common.h>
#include <imp/imp_audio.h>

#include "audio_capture.h"
#include "mkv_recorder.h"

#define TAG "audio"
#define AI_CHN     0
#define AENC_CHN   0

static volatile int g_running = 0;
static int g_initialized = 0;
static int g_thread_started = 0;
static pthread_t g_thread;
static int g_dev_id = 1;
static int g_sample_rate = 8000;

static IMPAudioSampleRate sample_rate_to_enum(int sr)
{
	switch (sr) {
		case 96000: return AUDIO_SAMPLE_RATE_96000;
		case 48000: return AUDIO_SAMPLE_RATE_48000;
		case 44100: return AUDIO_SAMPLE_RATE_44100;
		case 24000: return AUDIO_SAMPLE_RATE_24000;
		case 16000: return AUDIO_SAMPLE_RATE_16000;
		case 8000:
		default:    return AUDIO_SAMPLE_RATE_8000;
	}
}

static void teardown_imp_audio(int dev)
{
	if (!g_initialized)
		return;
	IMP_AENC_DestroyChn(AENC_CHN);
	IMP_AI_DisableChn(dev, AI_CHN);
	IMP_AI_Disable(dev);
	g_initialized = 0;
}

static void *audio_capture_thread(void *arg)
{
	(void)arg;
	int dev = g_dev_id;

	printf("[%s] capture thread started (dev=%d rate=%d)\n", TAG, dev, g_sample_rate);

	while (g_running) {
		/* Wait for a captured PCM frame from the mic */
		if (IMP_AI_PollingFrame(dev, AI_CHN, 500) != 0)
			continue;

		IMPAudioFrame frm;
		memset(&frm, 0, sizeof(frm));
		if (IMP_AI_GetFrame(dev, AI_CHN, &frm, BLOCK) != 0)
			continue;

		/* Encode PCM → G.711A, then release the input frame */
		int enc_ret = IMP_AENC_SendFrame(AENC_CHN, &frm);
		IMP_AI_ReleaseFrame(dev, AI_CHN, &frm);
		if (enc_ret != 0)
			continue;

		/* Pull the encoded G.711A stream and mux it */
		if (IMP_AENC_PollingStream(AENC_CHN, 500) != 0)
			continue;

		IMPAudioStream astream;
		memset(&astream, 0, sizeof(astream));
		if (IMP_AENC_GetStream(AENC_CHN, &astream, BLOCK) != 0)
			continue;

		mkv_recorder_write_audio_frame(
				(const uint8_t *)astream.stream, astream.len, astream.timeStamp);

		IMP_AENC_ReleaseStream(AENC_CHN, &astream);
	}

	printf("[%s] capture thread exiting\n", TAG);
	return NULL;
}

int audio_capture_start(const audio_capture_config_t *config)
{
	int ret;
	int dev;

	if (!config || !config->enabled)
		return 0;

	g_dev_id = config->dev_id;
	g_sample_rate = config->sample_rate ? config->sample_rate : 8000;
	dev = g_dev_id;

	IMPAudioIOAttr attr;
	memset(&attr, 0, sizeof(attr));
	attr.samplerate = sample_rate_to_enum(g_sample_rate);
	attr.bitwidth = AUDIO_BIT_WIDTH_16;
	attr.soundmode = (config->channels >= 2) ? AUDIO_SOUND_MODE_STEREO : AUDIO_SOUND_MODE_MONO;
	attr.frmNum = 4;
	attr.numPerFrm = g_sample_rate / 25;   /* ~40 ms frames (320 samples @ 8 kHz) */
	attr.chnCnt = 1;

	/* AGC must annotate attr BEFORE SetPubAttr so the device enables it.
	 * Non-fatal: audio still records if AGC isn't available. */
	IMPAudioAgcConfig agc;
	memset(&agc, 0, sizeof(agc));
	agc.TargetLevelDbfs = 1;      /* target near full scale (smaller = louder) */
	agc.CompressionGaindB = 24;   /* up to 24 dB of gain */
	int agc_ret = IMP_AI_EnableAgc(&attr, agc);
	if (agc_ret != 0)
		printf("[%s] IMP_AI_EnableAgc failed: %d (continuing without AGC)\n", TAG, agc_ret);
	else
		printf("[%s] AGC enabled (target=1 gain=24dB)\n", TAG);

	ret = IMP_AI_SetPubAttr(dev, &attr);
	if (ret != 0) {
		printf("[%s] IMP_AI_SetPubAttr failed: %d\n", TAG, ret);
		return -1;
	}

	ret = IMP_AI_Enable(dev);
	if (ret != 0) {
		printf("[%s] IMP_AI_Enable failed: %d\n", TAG, ret);
		return -1;
	}

	/* IMP_AI_SetChnParam MUST be called before IMP_AI_EnableChn (sets the
	 * frame cache depth); without it EnableChn is rejected. */
	IMPAudioIChnParam chnParam;
	memset(&chnParam, 0, sizeof(chnParam));
	chnParam.usrFrmDepth = 2;
	ret = IMP_AI_SetChnParam(dev, AI_CHN, &chnParam);
	if (ret != 0) {
		printf("[%s] IMP_AI_SetChnParam failed: %d\n", TAG, ret);
		IMP_AI_Disable(dev);
		return -1;
	}

	ret = IMP_AI_EnableChn(dev, AI_CHN);
	if (ret != 0) {
		printf("[%s] IMP_AI_EnableChn failed: %d\n", TAG, ret);
		IMP_AI_Disable(dev);
		return -1;
	}

	IMPAudioEncChnAttr enc;
	memset(&enc, 0, sizeof(enc));
	enc.type = PT_G711A;
	enc.bufSize = 4;
	enc.value = NULL;

	ret = IMP_AENC_CreateChn(AENC_CHN, &enc);
	if (ret != 0) {
		printf("[%s] IMP_AENC_CreateChn failed: %d\n", TAG, ret);
		IMP_AI_DisableChn(dev, AI_CHN);
		IMP_AI_Disable(dev);
		return -1;
	}
	g_initialized = 1;

	g_running = 1;
	ret = pthread_create(&g_thread, NULL, audio_capture_thread, NULL);
	if (ret != 0) {
		printf("[%s] pthread_create failed: %d\n", TAG, ret);
		g_running = 0;
		teardown_imp_audio(dev);
		return -1;
	}
	g_thread_started = 1;

	printf("[%s] started: dev=%d rate=%d G.711A\n", TAG, dev, g_sample_rate);
	return 0;
}

void audio_capture_stop(void)
{
	if (g_thread_started) {
		g_running = 0;
		pthread_join(g_thread, NULL);
		g_thread_started = 0;
	}

	teardown_imp_audio(g_dev_id);

	printf("[%s] stopped\n", TAG);
}
