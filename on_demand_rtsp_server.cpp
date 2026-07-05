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
#include <sys/wait.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "capture_and_encoding.h"
#include "imp-common.h"
#include "mkv_recorder.h"
#include "audio_capture.h"
#include "http_server.h"
#include "grafana_push.h"
#include "file_uploader.h"
#include "https_client.h"
#include "version.h"

static volatile int g_running = 1;
static volatile pid_t g_rtsp_child = -1;
static char g_camera_name[64] = "unknown";

static void signal_handler(int sig)
{
	/* Use write() instead of printf() — async-signal-safe */
	const char msg[] = "Caught signal, shutting down...\n";
	write(STDOUT_FILENO, msg, sizeof(msg) - 1);
	g_running = 0;
}

static void sigchld_handler(int sig)
{
	/* Reap zombie child processes */
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;
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

	/* Disable stdout buffering so logs appear immediately in log files,
	 * even if the process crashes before flushing */
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	/* Write version file */
	char const* versionFileName = "/tmp/version";
	int ver_fd = open(versionFileName, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (ver_fd >= 0) {
		write(ver_fd, VERSION, strlen(VERSION));
		close(ver_fd);
	}
	printf("t20-rtspd version: %s (pid %d)\n", VERSION, getpid());

	/* Install signal handlers for clean shutdown (use sigaction for portability) */
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	/* SIGCHLD handler to reap RTSP child process */
	sa.sa_handler = sigchld_handler;
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigaction(SIGCHLD, &sa, NULL);

	/* Step 1: Parse INI config */
	app_config_t config;
	if (app_config_parse("test.ini", &config) < 0) {
		printf("Failed to parse config\n");
		return 1;
	}

	/* Read camera name */
	{
		FILE *f = fopen("/system/sdcard/cameraname", "r");
		if (f) {
			if (fgets(g_camera_name, sizeof(g_camera_name), f)) {
				/* Trim trailing newline */
				size_t len = strlen(g_camera_name);
				while (len > 0 && (g_camera_name[len-1] == '\n' || g_camera_name[len-1] == '\r'))
					g_camera_name[--len] = '\0';
			}
			fclose(f);
		}
	}

	/* Pass config to encoder init */
	sample_encoder_set_config(&config);

	/* Step 2: Initialize IMP SDK (capture and encoding) */
	printf("[main] Initializing IMP SDK...\n");
	ret = capture_and_encoding();
	if (ret < 0) {
		printf("[main] capture_and_encoding() failed\n");
		return 1;
	}
	printf("[main] IMP SDK initialized\n");

	/* Step 3: If RTSP enabled, create FIFO and fork child for live555 */
	if (config.rtsp_enabled) {
		printf("[main] RTSP enabled, creating FIFO %s\n", inputFileName);

		unlink(inputFileName);
		if (mkfifo(inputFileName, 0600) < 0) {
			printf("[main] mkfifo(%s) failed: %s\n", inputFileName, strerror(errno));
			return 1;
		}

		printf("[main] Forking RTSP server child...\n");
		pid_t pid = fork();
		if (pid < 0) {
			printf("[main] fork() failed: %s\n", strerror(errno));
			return 1;
		}

		if (pid == 0) {
			/* Child process: RTSP server */
			printf("[rtsp] Child started (pid %d)\n", getpid());
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
		printf("[main] Parent (pid %d), RTSP child pid %d\n", getpid(), pid);
		g_rtsp_child = pid;
		fifo_fd = open(inputFileName, O_WRONLY);
		if (fifo_fd < 0) {
			printf("[main] Failed to open FIFO: %s\n", strerror(errno));
			return 1;
		}
		printf("[main] FIFO fd=%d opened for writing\n", fifo_fd);
	} else {
		printf("[main] RTSP server disabled\n");
	}

	/* Step 4: Init MKV recorder */
	printf("[main] Recording %s\n", config.recording_enabled ? "enabled" : "disabled");
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
		rec_config.audio_enabled = config.audio_enabled;
		rec_config.audio_sample_rate = config.audio_sample_rate;
		rec_config.audio_channels = 1;  /* mono */

		ret = mkv_recorder_init(&rec_config);
		if (ret < 0) {
			printf("[main] mkv_recorder_init() failed\n");
			return 1;
		}
		printf("[main] MKV recorder initialized\n");

		/* Start audio capture thread (mic → G.711A → recorder) */
		if (config.audio_enabled) {
			audio_capture_config_t acfg;
			memset(&acfg, 0, sizeof(acfg));
			acfg.enabled = 1;
			acfg.dev_id = config.audio_dev_id;
			acfg.sample_rate = config.audio_sample_rate;
			acfg.channels = 1;  /* mono */

			ret = audio_capture_start(&acfg);
			if (ret < 0)
				printf("[main] audio_capture_start() failed (non-fatal)\n");
			else
				printf("[main] Audio capture started\n");
		}
	}

	/* Step 4c: Init file uploader */
	if (config.upload_enabled) {
		file_uploader_config_t ul_config;
		memset(&ul_config, 0, sizeof(ul_config));
		ul_config.enabled = config.upload_enabled;
		strncpy(ul_config.upload_url, config.upload_url, sizeof(ul_config.upload_url) - 1);
		strncpy(ul_config.upload_token, config.upload_token, sizeof(ul_config.upload_token) - 1);
		strncpy(ul_config.scan_dir, config.recording_output_dir, sizeof(ul_config.scan_dir) - 1);
		ul_config.rate_limit_kbps = config.upload_rate_limit_kbps;
		ul_config.scan_interval_s = config.upload_scan_interval_s;
		ul_config.buffer_in_memory = config.upload_buffer_in_memory;
		ul_config.adaptive_rate = config.upload_adaptive_rate;

		ret = file_uploader_init(&ul_config);
		if (ret < 0)
			printf("[main] file_uploader_init() failed (non-fatal)\n");
	}

	/* Step 4d: Start HTTP server for snapshots (optional) */
	if (config.http_enabled) {
		/* Initialize JPEG encoder for snapshots */
		extern int sample_jpeg_init(void);
		if (sample_jpeg_init() < 0)
			printf("[main] sample_jpeg_init() failed (snapshots disabled)\n");
		else
			printf("[main] JPEG encoder initialized for snapshots\n");

		/* Set callbacks for status endpoint */
		extern void http_server_set_callbacks(const char *(*)(void), int (*)(void));
		http_server_set_callbacks(
			[]() -> const char* { return g_camera_name; },
			[]() -> int { return (int)time(NULL); }
		);
		ret = http_server_start(config.http_port);
		if (ret < 0)
			printf("[main] http_server_start() failed (non-fatal)\n");
		else
			printf("[main] HTTP snapshot server started on port %d\n", config.http_port);

		/* Restart mDNS to advertise snapshot URL in TXT records */
		char mdnsCmd[512];
		snprintf(mdnsCmd, sizeof(mdnsCmd),
			"killall mDNSResponder 2>/dev/null; "
			"sleep 1; "
			"/system/sdcard/bin/mDNSResponder -b -P /run/mdns-responder.pid "
			"-n %s -t _rtsp._tcp -p %d "
			"-x path=/snapshot -x name=%s -x id=%s 2>/dev/null &",
			g_camera_name, config.http_port, g_camera_name, g_camera_name);
		system(mdnsCmd);
	}

	/* Step 5: Start receiving encoded frames */
	printf("[main] Starting encoder receive...\n");
	ret = start_encoder_receiving(0);
	if (ret < 0) {
		printf("[main] start_encoder_receiving() failed\n");
		return 1;
	}

	/* Step 6: Main capture loop */
	printf("[main] Entering main capture loop\n");
	IMPEncoderStream stream;
	unsigned long frame_count = 0;
	unsigned long poll_timeouts = 0;
	unsigned long getstream_errors = 0;
	unsigned long stats_frame_threshold = 300; /* ~60s at 5fps */
	unsigned long next_stats_frame = stats_frame_threshold;

	while (g_running) {
		/* Poll for encoded stream — match frame interval to reduce syscalls */
		ret = IMP_Encoder_PollingStream(0, 500);
		if (ret < 0) {
			poll_timeouts++;
			continue; /* timeout, try again */
		}

		ret = IMP_Encoder_GetStream(0, &stream, 1);
		if (ret < 0) {
			getstream_errors++;
			if (getstream_errors <= 10 || (getstream_errors % 100) == 0)
				printf("[main] IMP_Encoder_GetStream() failed (count=%lu)\n", getstream_errors);
			continue;
		}

		/* Feed to MKV recorder */
		if (config.recording_enabled)
			mkv_recorder_write_frame(&stream);

		/* Feed to RTSP FIFO if enabled */
		if (config.rtsp_enabled && fifo_fd >= 0)
			save_stream_to_fd(fifo_fd, &stream);

		IMP_Encoder_ReleaseStream(0, &stream);
		frame_count++;

		/* Periodic stats — use frame count instead of time() to avoid syscall */
		if (frame_count >= next_stats_frame) {
			printf("[main] Stats: frames=%lu poll_timeouts=%lu getstream_errors=%lu\n",
					frame_count, poll_timeouts, getstream_errors);
			next_stats_frame = frame_count + stats_frame_threshold;
		}
	}

	/* Clean shutdown */
	printf("[main] Shutting down (frames=%lu)...\n", frame_count);

	if (config.upload_enabled)
		file_uploader_shutdown();

	if (config.grafana_enabled)
		grafana_push_shutdown();

	/* Stop audio before the recorder so no audio writes land during teardown */
	if (config.recording_enabled && config.audio_enabled)
		audio_capture_stop();

	/* Stop HTTP server */
	if (config.http_enabled)
		http_server_stop();

	if (config.recording_enabled)
		mkv_recorder_shutdown();

	if (fifo_fd >= 0)
		close(fifo_fd);

	/* Kill RTSP child process */
	if (g_rtsp_child > 0) {
		kill(g_rtsp_child, SIGTERM);
		waitpid(g_rtsp_child, NULL, 0);
	}

	IMP_Encoder_StopRecvPic(0);

	/* Teardown IMP SDK resources */
	destory();

	https_client_cleanup();

	return 0;
}
