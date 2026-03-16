#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <time.h>

#include "file_uploader.h"
#include "https_client.h"

#define TAG "file_uploader"
#define MAX_FILES 64

static file_uploader_config_t g_config;
static pthread_t g_thread;
static volatile int g_running = 0;
static char g_camera_name[64];
static char g_local_ip[32];
static char g_wifi_ssid[64];
static char g_auth_header[320];

/* --- Read camera identity info --- */

static void read_camera_name(void)
{
	FILE *f = fopen("/system/sdcard/cameraname", "r");
	if (f) {
		if (fgets(g_camera_name, sizeof(g_camera_name), f)) {
			int len = strlen(g_camera_name);
			while (len > 0 && (g_camera_name[len - 1] == '\n' ||
					g_camera_name[len - 1] == '\r' ||
					g_camera_name[len - 1] == ' '))
				g_camera_name[--len] = '\0';
			if (len > 0) {
				fclose(f);
				return;
			}
		}
		fclose(f);
	}

	if (gethostname(g_camera_name, sizeof(g_camera_name)) != 0)
		strncpy(g_camera_name, "unknown", sizeof(g_camera_name) - 1);
}

static void read_local_ip(void)
{
	int fd;
	struct ifreq ifr;

	strncpy(g_local_ip, "0.0.0.0", sizeof(g_local_ip));

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ - 1);

	if (ioctl(fd, SIOCGIFADDR, &ifr) == 0) {
		struct sockaddr_in *addr = (struct sockaddr_in *)&ifr.ifr_addr;
		strncpy(g_local_ip, inet_ntoa(addr->sin_addr), sizeof(g_local_ip) - 1);
	}

	close(fd);
}

static void read_wifi_ssid(void)
{
	FILE *f;
	char line[256];

	strncpy(g_wifi_ssid, "unknown", sizeof(g_wifi_ssid));

	f = fopen("/proc/net/wireless", "r");
	if (!f)
		return;
	fclose(f);

	/* Try reading SSID from iwconfig-style /proc or wpa_supplicant */
	f = popen("iwconfig wlan0 2>/dev/null | grep ESSID", "r");
	if (f) {
		if (fgets(line, sizeof(line), f)) {
			char *start = strstr(line, "ESSID:\"");
			if (start) {
				start += 7;
				char *end = strchr(start, '"');
				if (end) {
					int len = end - start;
					if (len >= (int)sizeof(g_wifi_ssid))
						len = sizeof(g_wifi_ssid) - 1;
					memcpy(g_wifi_ssid, start, len);
					g_wifi_ssid[len] = '\0';
				}
			}
		}
		pclose(f);
	}
}

/* --- File scanning --- */

typedef struct {
	char name[256];
	time_t mtime;
} file_entry_t;

static int compare_mtime(const void *a, const void *b)
{
	const file_entry_t *fa = (const file_entry_t *)a;
	const file_entry_t *fb = (const file_entry_t *)b;
	if (fa->mtime < fb->mtime) return -1;
	if (fa->mtime > fb->mtime) return 1;
	return 0;
}

static int scan_completed_files(const char *dir, file_entry_t *files, int max_files)
{
	DIR *d;
	struct dirent *ent;
	struct stat st;
	char filepath[512];
	int count = 0;
	time_t now;

	time(&now);

	d = opendir(dir);
	if (!d) {
		printf("[%s] opendir(%s) failed\n", TAG, dir);
		return 0;
	}

	while ((ent = readdir(d)) != NULL && count < max_files) {
		int len = strlen(ent->d_name);
		if (len < 5 || strcmp(ent->d_name + len - 4, ".mkv") != 0)
			continue;

		snprintf(filepath, sizeof(filepath), "%s/%s", dir, ent->d_name);

		if (stat(filepath, &st) != 0)
			continue;

		/* Skip files smaller than 100KB (corrupt/incomplete) */
		if (st.st_size < 100 * 1024)
			continue;

		/* Skip files modified in the last 30 seconds (still being written) */
		if (now - st.st_mtime < 30)
			continue;

		strncpy(files[count].name, ent->d_name, sizeof(files[count].name) - 1);
		files[count].name[sizeof(files[count].name) - 1] = '\0';
		files[count].mtime = st.st_mtime;
		count++;
	}

	closedir(d);

	/* Sort oldest first */
	if (count > 1)
		qsort(files, count, sizeof(file_entry_t), compare_mtime);

	return count;
}

/* --- Upload thread --- */

static void *file_uploader_thread(void *arg)
{
	int upload_count = 0;
	(void)arg;

	printf("[%s] Upload thread started (scan_dir=%s interval=%ds rate=%dKB/s)\n",
			TAG, g_config.scan_dir, g_config.scan_interval_s,
			g_config.rate_limit_kbps);

	while (g_running) {
		/* Refresh IP and SSID each round (can change) */
		read_local_ip();
		read_wifi_ssid();

		file_entry_t files[MAX_FILES];
		int nfiles = scan_completed_files(g_config.scan_dir, files, MAX_FILES);

		if (nfiles > 0)
			printf("[%s] Found %d completed file(s) to upload\n", TAG, nfiles);

		int i;
		for (i = 0; i < nfiles && g_running; i++) {
			char filepath[512];
			snprintf(filepath, sizeof(filepath), "%s/%s",
					g_config.scan_dir, files[i].name);

			/* Build filename with camera name prefix */
			char dest_filename[320];
			if (strncmp(files[i].name, g_camera_name, strlen(g_camera_name)) == 0)
				snprintf(dest_filename, sizeof(dest_filename), "%s", files[i].name);
			else
				snprintf(dest_filename, sizeof(dest_filename), "%s%s",
						g_camera_name, files[i].name);

			const char *headers[] = {
				g_auth_header,
				"Content-Type: application/octet-stream",
				NULL, NULL, NULL, NULL, NULL
			};

			/* Build dynamic headers */
			char h_camera[128], h_ip[64], h_filename[384], h_wifi[128];
			snprintf(h_camera, sizeof(h_camera), "X-Camera-Name: %s", g_camera_name);
			snprintf(h_ip, sizeof(h_ip), "X-Camera-IP: %s", g_local_ip);
			snprintf(h_filename, sizeof(h_filename), "X-Filename: %s", dest_filename);
			snprintf(h_wifi, sizeof(h_wifi), "X-Camera-Wifi: %s", g_wifi_ssid);
			headers[2] = h_camera;
			headers[3] = h_ip;
			headers[4] = h_filename;
			headers[5] = h_wifi;

			printf("[%s] Uploading %s (%d/%d)...\n", TAG, files[i].name, i + 1, nfiles);

			int http_status = 0;
			int ret = https_post_file(g_config.upload_url, headers,
					filepath, g_config.rate_limit_kbps, &http_status);

			if (ret == 0) {
				upload_count++;
				printf("[%s] Upload OK: %s (http=%d, total=%d)\n",
						TAG, files[i].name, http_status, upload_count);
				if (unlink(filepath) != 0)
					printf("[%s] Failed to delete %s after upload\n", TAG, filepath);
			} else {
				printf("[%s] Upload FAILED: %s (http=%d), will retry\n",
						TAG, files[i].name, http_status);
			}
		}

		/* Sleep in 1s increments for quick shutdown */
		int remaining = g_config.scan_interval_s;
		while (remaining > 0 && g_running) {
			sleep(1);
			remaining--;
		}
	}

	printf("[%s] Upload thread exiting (uploaded %d files)\n", TAG, upload_count);
	return NULL;
}

/* --- Public API --- */

int file_uploader_init(const file_uploader_config_t *config)
{
	if (!config || !config->enabled) {
		printf("[%s] File upload disabled\n", TAG);
		return 0;
	}

	if (strlen(config->upload_url) == 0) {
		printf("[%s] No upload URL configured, disabling\n", TAG);
		return 0;
	}

	memcpy(&g_config, config, sizeof(file_uploader_config_t));

	read_camera_name();
	read_local_ip();
	read_wifi_ssid();

	printf("[%s] Camera: %s IP: %s WiFi: %s\n", TAG,
			g_camera_name, g_local_ip, g_wifi_ssid);

	/* Build auth header */
	snprintf(g_auth_header, sizeof(g_auth_header),
			"Authorization: Bearer %s", g_config.upload_token);

	/* Ensure HTTPS client is initialized */
	if (https_client_init() < 0) {
		printf("[%s] https_client_init() failed\n", TAG);
		return -1;
	}

	g_running = 1;
	int ret = pthread_create(&g_thread, NULL, file_uploader_thread, NULL);
	if (ret != 0) {
		printf("[%s] Failed to create upload thread: %d\n", TAG, ret);
		g_running = 0;
		return -1;
	}

	printf("[%s] Initialized: url=%s rate=%dKB/s\n",
			TAG, g_config.upload_url, g_config.rate_limit_kbps);
	return 0;
}

void file_uploader_shutdown(void)
{
	if (!g_running)
		return;

	printf("[%s] Shutting down\n", TAG);
	g_running = 0;
	pthread_join(g_thread, NULL);
	printf("[%s] Shutdown complete\n", TAG);
}
