#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <errno.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>

#include "mkv_recorder.h"

#define TAG "mkv_recorder"

static mkv_recorder_config_t g_config;
static AVFormatContext *g_fmt_ctx = NULL;
static AVStream *g_video_stream = NULL;

static uint8_t *g_sps_data = NULL;
static int g_sps_size = 0;
static uint8_t *g_pps_data = NULL;
static int g_pps_size = 0;
static int g_got_extradata = 0;

static int64_t g_chunk_start_time = 0;  /* seconds since epoch */
static int64_t g_frame_count = 0;
static int64_t g_first_pts = -1;

static uint8_t *g_frame_buf = NULL;
static int g_frame_buf_size = 0;

static int check_disk_usage(const char *path)
{
	struct statvfs stat;

	if (statvfs(path, &stat) != 0) {
		printf("[%s] statvfs(%s) failed: %s\n", TAG, path, strerror(errno));
		return -1;
	}

	unsigned long total = stat.f_blocks;
	unsigned long avail = stat.f_bavail;

	if (total == 0)
		return 0;

	int usage_pct = (int)(((total - avail) * 100) / total);
	return usage_pct;
}

static int open_new_chunk(void)
{
	int ret;
	char filepath[512];
	time_t now;
	struct tm *tm_info;

	/* Check disk usage */
	int usage = check_disk_usage(g_config.output_dir);
	if (usage >= 0 && usage >= g_config.disk_usage_threshold) {
		printf("[%s] Disk usage %d%% >= threshold %d%%, skipping recording\n",
				TAG, usage, g_config.disk_usage_threshold);
		return -1;
	}

	/* Generate filename with timestamp */
	time(&now);
	tm_info = localtime(&now);
	snprintf(filepath, sizeof(filepath), "%s/%04d%02d%02d_%02d%02d%02d.mkv",
			g_config.output_dir,
			tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
			tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);

	printf("[%s] Opening new chunk: %s\n", TAG, filepath);

	/* Allocate output context */
	ret = avformat_alloc_output_context2(&g_fmt_ctx, NULL, "matroska", filepath);
	if (ret < 0 || !g_fmt_ctx) {
		printf("[%s] avformat_alloc_output_context2 failed: %d\n", TAG, ret);
		return -1;
	}

	/* Create video stream */
	g_video_stream = avformat_new_stream(g_fmt_ctx, NULL);
	if (!g_video_stream) {
		printf("[%s] avformat_new_stream failed\n", TAG);
		avformat_free_context(g_fmt_ctx);
		g_fmt_ctx = NULL;
		return -1;
	}

	g_video_stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
	g_video_stream->codecpar->codec_id = AV_CODEC_ID_H264;
	g_video_stream->codecpar->width = g_config.width;
	g_video_stream->codecpar->height = g_config.height;

	g_video_stream->time_base.num = g_config.fps_den;
	g_video_stream->time_base.den = g_config.fps_num;

	/* Set extradata (SPS + PPS) if available */
	if (g_got_extradata && g_sps_data && g_pps_data) {
		/* Format: [0x00 0x00 0x00 0x01] SPS [0x00 0x00 0x00 0x01] PPS */
		int extradata_size = 4 + g_sps_size + 4 + g_pps_size;
		uint8_t *extradata = (uint8_t *)av_mallocz(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
		if (extradata) {
			int offset = 0;
			extradata[offset++] = 0x00;
			extradata[offset++] = 0x00;
			extradata[offset++] = 0x00;
			extradata[offset++] = 0x01;
			memcpy(extradata + offset, g_sps_data, g_sps_size);
			offset += g_sps_size;
			extradata[offset++] = 0x00;
			extradata[offset++] = 0x00;
			extradata[offset++] = 0x00;
			extradata[offset++] = 0x01;
			memcpy(extradata + offset, g_pps_data, g_pps_size);

			g_video_stream->codecpar->extradata = extradata;
			g_video_stream->codecpar->extradata_size = extradata_size;
		}
	}

	/* Open output file */
	ret = avio_open(&g_fmt_ctx->pb, filepath, AVIO_FLAG_WRITE);
	if (ret < 0) {
		printf("[%s] avio_open failed: %d\n", TAG, ret);
		avformat_free_context(g_fmt_ctx);
		g_fmt_ctx = NULL;
		return -1;
	}

	/* Write header */
	ret = avformat_write_header(g_fmt_ctx, NULL);
	if (ret < 0) {
		printf("[%s] avformat_write_header failed: %d\n", TAG, ret);
		avio_closep(&g_fmt_ctx->pb);
		avformat_free_context(g_fmt_ctx);
		g_fmt_ctx = NULL;
		return -1;
	}

	time(&now);
	g_chunk_start_time = (int64_t)now;
	g_frame_count = 0;
	g_first_pts = -1;

	printf("[%s] Chunk opened successfully\n", TAG);
	return 0;
}

static void close_current_chunk(void)
{
	if (!g_fmt_ctx)
		return;

	printf("[%s] Closing chunk (%lld frames)\n", TAG, (long long)g_frame_count);

	av_write_trailer(g_fmt_ctx);
	avio_closep(&g_fmt_ctx->pb);
	avformat_free_context(g_fmt_ctx);
	g_fmt_ctx = NULL;
	g_video_stream = NULL;
	g_first_pts = -1;
}

static int is_idr_frame(IMPEncoderStream *stream)
{
	int i;
	for (i = 0; i < (int)stream->packCount; i++) {
		if (stream->pack[i].dataType.h264Type == IMP_H264_NAL_SLICE_IDR)
			return 1;
	}
	return 0;
}

static void cache_sps_pps(IMPEncoderStream *stream)
{
	int i;
	for (i = 0; i < (int)stream->packCount; i++) {
		IMPEncoderPack *pack = &stream->pack[i];
		IMPEncoderH264NaluType nal_type = pack->dataType.h264Type;

		if (nal_type == IMP_H264_NAL_SPS) {
			if (g_sps_data)
				free(g_sps_data);
			g_sps_data = (uint8_t *)malloc(pack->length);
			if (g_sps_data) {
				memcpy(g_sps_data, (void *)pack->virAddr, pack->length);
				g_sps_size = pack->length;
			}
		} else if (nal_type == IMP_H264_NAL_PPS) {
			if (g_pps_data)
				free(g_pps_data);
			g_pps_data = (uint8_t *)malloc(pack->length);
			if (g_pps_data) {
				memcpy(g_pps_data, (void *)pack->virAddr, pack->length);
				g_pps_size = pack->length;
			}
		}
	}

	if (g_sps_data && g_pps_data)
		g_got_extradata = 1;
}

int mkv_recorder_init(const mkv_recorder_config_t *config)
{
	struct stat st;

	if (!config) {
		printf("[%s] NULL config\n", TAG);
		return -1;
	}

	memcpy(&g_config, config, sizeof(mkv_recorder_config_t));

	if (!g_config.enabled) {
		printf("[%s] Recording disabled\n", TAG);
		return 0;
	}

	/* Ensure output directory exists */
	if (stat(g_config.output_dir, &st) != 0) {
		printf("[%s] Output directory %s does not exist, attempting to create\n",
				TAG, g_config.output_dir);
		if (mkdir(g_config.output_dir, 0755) != 0 && errno != EEXIST) {
			printf("[%s] Failed to create output directory: %s\n",
					TAG, strerror(errno));
			return -1;
		}
	}

#if LIBAVFORMAT_VERSION_MAJOR < 58
	av_register_all();
#endif

	printf("[%s] Recorder initialized: dir=%s chunk=%ds threshold=%d%%\n",
			TAG, g_config.output_dir, g_config.chunk_duration,
			g_config.disk_usage_threshold);
	printf("[%s] Resolution: %dx%d @ %d/%d fps\n",
			TAG, g_config.width, g_config.height,
			g_config.fps_num, g_config.fps_den);

	return 0;
}

int mkv_recorder_write_frame(IMPEncoderStream *stream)
{
	int ret;
	int idr;
	time_t now;

	if (!g_config.enabled)
		return 0;

	/* Always try to cache SPS/PPS from incoming stream */
	cache_sps_pps(stream);

	/* Wait until we have extradata before opening first chunk */
	if (!g_got_extradata)
		return 0;

	idr = is_idr_frame(stream);

	/* Check if we need to rotate chunks */
	if (g_fmt_ctx && idr) {
		time(&now);
		if ((int64_t)now - g_chunk_start_time >= g_config.chunk_duration) {
			close_current_chunk();
		}
	}

	/* Open new chunk if needed (only at IDR boundaries) */
	if (!g_fmt_ctx && idr) {
		ret = open_new_chunk();
		if (ret < 0)
			return 0; /* Skip frames until we can open a chunk */
	}

	/* If still no context, skip this frame */
	if (!g_fmt_ctx)
		return 0;

	/* Assemble all packs into a single buffer for this frame */
	int total_size = 0;
	int i;
	for (i = 0; i < (int)stream->packCount; i++)
		total_size += stream->pack[i].length;

	if (total_size <= 0)
		return 0;

	/* Grow reusable frame buffer if needed */
	if (total_size > g_frame_buf_size) {
		free(g_frame_buf);
		g_frame_buf = (uint8_t *)malloc(total_size);
		if (!g_frame_buf) {
			printf("[%s] malloc(%d) failed\n", TAG, total_size);
			g_frame_buf_size = 0;
			return -1;
		}
		g_frame_buf_size = total_size;
	}

	int offset = 0;
	for (i = 0; i < (int)stream->packCount; i++) {
		memcpy(g_frame_buf + offset, (void *)stream->pack[i].virAddr, stream->pack[i].length);
		offset += stream->pack[i].length;
	}

	AVPacket pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.data = g_frame_buf;
	pkt.size = total_size;
	pkt.stream_index = g_video_stream->index;

	/* Timestamp handling */
	int64_t encoder_ts = stream->pack[0].timestamp; /* microseconds */
	if (g_first_pts < 0)
		g_first_pts = encoder_ts;

	int64_t relative_ts = encoder_ts - g_first_pts;

	/* Convert microseconds to stream time_base */
	AVRational us_tb = {1, 1000000};
	pkt.pts = av_rescale_q(relative_ts, us_tb, g_video_stream->time_base);
	pkt.dts = pkt.pts;
	pkt.duration = av_rescale_q(1, (AVRational){g_config.fps_den, g_config.fps_num},
			g_video_stream->time_base);

	if (idr)
		pkt.flags |= AV_PKT_FLAG_KEY;

	ret = av_interleaved_write_frame(g_fmt_ctx, &pkt);

	if (ret < 0) {
		printf("[%s] av_interleaved_write_frame failed: %d\n", TAG, ret);
		/* Close broken chunk; next IDR will open a fresh file */
		close_current_chunk();
		return -1;
	}

	g_frame_count++;
	return 0;
}

void mkv_recorder_shutdown(void)
{
	printf("[%s] Shutting down recorder\n", TAG);

	close_current_chunk();

	if (g_sps_data) {
		free(g_sps_data);
		g_sps_data = NULL;
	}
	if (g_pps_data) {
		free(g_pps_data);
		g_pps_data = NULL;
	}
	g_sps_size = 0;
	g_pps_size = 0;
	g_got_extradata = 0;

	if (g_frame_buf) {
		free(g_frame_buf);
		g_frame_buf = NULL;
		g_frame_buf_size = 0;
	}
}

int64_t mkv_recorder_get_frame_count(void)
{
	return g_frame_count;
}

int64_t mkv_recorder_get_chunk_start_time(void)
{
	return g_chunk_start_time;
}

int mkv_recorder_get_disk_usage(int *usage_pct, unsigned long *free_kb)
{
	struct statvfs stat;

	if (statvfs(g_config.output_dir, &stat) != 0)
		return -1;

	unsigned long total = stat.f_blocks;
	unsigned long avail = stat.f_bavail;

	if (usage_pct) {
		if (total == 0)
			*usage_pct = 0;
		else
			*usage_pct = (int)(((total - avail) * 100) / total);
	}
	if (free_kb)
		*free_kb = (unsigned long)((avail * stat.f_frsize) / 1024);

	return 0;
}
