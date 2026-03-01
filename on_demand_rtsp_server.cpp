/**********
This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the
Free Software Foundation; either version 3 of the License, or (at your
option) any later version. (See <http://www.gnu.org/copyleft/lesser.html>.)

This library is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
more details.

You should have received a copy of the GNU Lesser General Public License
along with this library; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
**********/
// Copyright (c) 1996-2018, Live Networks, Inc.  All rights reserved

#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#include "capture_and_encoding.h"
#include "imp-common.h"
#include "mkv_recorder.h"
#include "grafana_push.h"
#include "version.h"

static volatile int g_running = 1;

static void signal_handler(int sig)
{
	printf("Caught signal %d, shutting down...\n", sig);
	g_running = 0;
}

static void announceStream(RTSPServer* rtspServer, ServerMediaSession* sms,
			   char const* streamName, char const* inputFileName) {
	char* url = rtspServer->rtspURL(sms);
	UsageEnvironment& env = rtspServer->envir();
	env << "\n\"" << streamName << "\" stream, from the file \""
	    << inputFileName << "\"\n";
	env << "Play this stream using the URL \"" << url << "\"\n";
	delete[] url;
}

int main(int argc, char** argv) {
	int ret;
	char const* inputFileName = "/tmp/h264_fifo";
	int fifo_fd = -1;

	/* Write version file */
	char const* versionFileName = "/tmp/version";
	int ver_fd = open(versionFileName, O_RDWR | O_CREAT | O_TRUNC, 0777);
	if (ver_fd >= 0) {
		write(ver_fd, VERSION, sizeof(VERSION));
		close(ver_fd);
	}
	printf("t20-rtspd version: %s\n", VERSION);

	/* Install signal handlers for clean shutdown */
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* Step 1: Parse INI config */
	app_config_t config;
	if (app_config_parse("test.ini", &config) < 0) {
		printf("Failed to parse config\n");
		return 1;
	}

	/* Pass config to encoder init */
	sample_encoder_set_config(&config);

	/* Step 2: Initialize IMP SDK (capture and encoding) */
	ret = capture_and_encoding();
	if (ret < 0) {
		printf("capture_and_encoding() failed\n");
		return 1;
	}

	/* Step 3: If RTSP enabled, create FIFO and fork child for live555 */
	if (config.rtsp_enabled) {
		printf("RTSP server enabled on port %d\n", config.rtsp_port);

		unlink(inputFileName);
		if (mkfifo(inputFileName, 0777) < 0) {
			printf("mkfifo failed\n");
			return 1;
		}

		pid_t pid = fork();
		if (pid < 0) {
			printf("fork() failed\n");
			return 1;
		}

		if (pid == 0) {
			/* Child process: RTSP server */
			TaskScheduler* scheduler = BasicTaskScheduler::createNew();
			UsageEnvironment* env = BasicUsageEnvironment::createNew(*scheduler);

			OutPacketBuffer::maxSize = 600000;

			UserAuthenticationDatabase* authDB = NULL;
#ifdef ACCESS_CONTROL
			authDB = new UserAuthenticationDatabase;
			authDB->addUserRecord("username1", "password1");
#endif

			RTSPServer* rtspServer = RTSPServer::createNew(*env, config.rtsp_port, authDB);
			if (rtspServer == NULL) {
				*env << "Failed to create RTSP server: " << env->getResultMsg() << "\n";
				exit(1);
			}

			char const* descriptionString = "Session streamed by \"t20-rtspd\"";
			char const* streamName = "unicast";
			ServerMediaSession* sms
			  = ServerMediaSession::createNew(*env, streamName, streamName,
							      descriptionString);
			sms->addSubsession(H264VideoFileServerMediaSubsession
				       ::createNew(*env, inputFileName, False));
			rtspServer->addServerMediaSession(sms);

			announceStream(rtspServer, sms, streamName, inputFileName);

			env->taskScheduler().doEventLoop(); /* does not return */
			return 0;
		}

		/* Parent process continues: open FIFO for writing */
		fifo_fd = open(inputFileName, O_RDWR | O_CREAT | O_TRUNC, 0777);
		if (fifo_fd < 0) {
			printf("Failed to open FIFO for writing\n");
			return 1;
		}
	} else {
		printf("RTSP server disabled\n");
	}

	/* Step 4: Init MKV recorder */
	if (config.recording_enabled) {
		mkv_recorder_config_t rec_config;
		rec_config.enabled = config.recording_enabled;
		strncpy(rec_config.output_dir, config.recording_output_dir, sizeof(rec_config.output_dir) - 1);
		rec_config.output_dir[sizeof(rec_config.output_dir) - 1] = '\0';
		rec_config.chunk_duration = config.recording_chunk_duration;
		rec_config.disk_usage_threshold = config.recording_disk_threshold;
		rec_config.width = config.WIDTH ? config.WIDTH : SENSOR_WIDTH;
		rec_config.height = config.HEIGHT ? config.HEIGHT : SENSOR_HEIGHT;
		rec_config.fps_num = config.RATENUM ? config.RATENUM : SENSOR_FRAME_RATE_NUM;
		rec_config.fps_den = config.RATEDEN ? config.RATEDEN : SENSOR_FRAME_RATE_DEN;

		ret = mkv_recorder_init(&rec_config);
		if (ret < 0) {
			printf("mkv_recorder_init() failed\n");
			return 1;
		}
	}

	/* Step 4b: Init Grafana metrics push */
	if (config.grafana_enabled) {
		grafana_push_config_t gf_config;
		gf_config.enabled = config.grafana_enabled;
		strncpy(gf_config.push_url, config.grafana_push_url, sizeof(gf_config.push_url) - 1);
		gf_config.push_url[sizeof(gf_config.push_url) - 1] = '\0';
		strncpy(gf_config.username, config.grafana_username, sizeof(gf_config.username) - 1);
		gf_config.username[sizeof(gf_config.username) - 1] = '\0';
		strncpy(gf_config.api_key, config.grafana_api_key, sizeof(gf_config.api_key) - 1);
		gf_config.api_key[sizeof(gf_config.api_key) - 1] = '\0';
		gf_config.push_interval_ms = config.grafana_push_interval_ms;

		ret = grafana_push_init(&gf_config);
		if (ret < 0)
			printf("grafana_push_init() failed (non-fatal)\n");
	}

	/* Step 5: Start receiving encoded frames */
	ret = start_encoder_receiving(0);
	if (ret < 0) {
		printf("start_encoder_receiving() failed\n");
		return 1;
	}

	/* Step 6: Main capture loop */
	printf("Entering main capture loop\n");
	IMPEncoderStream stream;
	while (g_running) {
		/* Poll for encoded stream */
		ret = IMP_Encoder_PollingStream(0, 100);
		if (ret < 0)
			continue; /* timeout, try again */

		ret = IMP_Encoder_GetStream(0, &stream, 1);
		if (ret < 0) {
			printf("IMP_Encoder_GetStream() failed\n");
			continue;
		}

		/* Feed to MKV recorder */
		if (config.recording_enabled)
			mkv_recorder_write_frame(&stream);

		/* Feed to RTSP FIFO if enabled */
		if (config.rtsp_enabled && fifo_fd >= 0)
			save_stream_to_fd(fifo_fd, &stream);

		IMP_Encoder_ReleaseStream(0, &stream);
	}

	/* Clean shutdown */
	printf("Shutting down...\n");

	if (config.grafana_enabled)
		grafana_push_shutdown();

	if (config.recording_enabled)
		mkv_recorder_shutdown();

	if (fifo_fd >= 0)
		close(fifo_fd);

	IMP_Encoder_StopRecvPic(0);

	return 0;
}
