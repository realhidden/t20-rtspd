#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/error.h>

#include "https_client.h"

#define TAG "https_client"
#define CA_CERT_PATH "/system/sdcard/config/cacert.pem"
#define READ_TIMEOUT_MS 10000
#define RESPONSE_BUF_SIZE 1024

/* Global state (initialized once) */
static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_ctr_drbg;
static mbedtls_x509_crt g_cacert;
static mbedtls_ssl_config g_ssl_conf;
static int g_initialized = 0;
static int g_has_cacert = 0;

int https_client_init(void)
{
	int ret;
	const char *pers = "t20_https";

	if (g_initialized)
		return 0;

	mbedtls_entropy_init(&g_entropy);
	mbedtls_ctr_drbg_init(&g_ctr_drbg);
	mbedtls_x509_crt_init(&g_cacert);
	mbedtls_ssl_config_init(&g_ssl_conf);

	/* Seed the random number generator */
	ret = mbedtls_ctr_drbg_seed(&g_ctr_drbg, mbedtls_entropy_func,
			&g_entropy, (const unsigned char *)pers, strlen(pers));
	if (ret != 0) {
		printf("[%s] ctr_drbg_seed failed: -0x%04x\n", TAG, -ret);
		return -1;
	}

	/* Try to load CA certificates */
	if (access(CA_CERT_PATH, R_OK) == 0) {
		ret = mbedtls_x509_crt_parse_file(&g_cacert, CA_CERT_PATH);
		if (ret == 0) {
			g_has_cacert = 1;
			printf("[%s] Loaded CA certs from %s\n", TAG, CA_CERT_PATH);
		} else {
			printf("[%s] Failed to parse %s: -0x%04x (continuing without verification)\n",
					TAG, CA_CERT_PATH, -ret);
		}
	} else {
		printf("[%s] No CA cert at %s, TLS verification disabled\n", TAG, CA_CERT_PATH);
	}

	/* Configure TLS */
	ret = mbedtls_ssl_config_defaults(&g_ssl_conf,
			MBEDTLS_SSL_IS_CLIENT,
			MBEDTLS_SSL_TRANSPORT_STREAM,
			MBEDTLS_SSL_PRESET_DEFAULT);
	if (ret != 0) {
		printf("[%s] ssl_config_defaults failed: -0x%04x\n", TAG, -ret);
		return -1;
	}

	mbedtls_ssl_conf_rng(&g_ssl_conf, mbedtls_ctr_drbg_random, &g_ctr_drbg);
	mbedtls_ssl_conf_read_timeout(&g_ssl_conf, READ_TIMEOUT_MS);

	if (g_has_cacert) {
		mbedtls_ssl_conf_ca_chain(&g_ssl_conf, &g_cacert, NULL);
		mbedtls_ssl_conf_authmode(&g_ssl_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	} else {
		mbedtls_ssl_conf_authmode(&g_ssl_conf, MBEDTLS_SSL_VERIFY_NONE);
	}

	g_initialized = 1;
	printf("[%s] Initialized (verify=%s)\n", TAG,
			g_has_cacert ? "required" : "none");
	return 0;
}

void https_client_cleanup(void)
{
	if (!g_initialized)
		return;

	mbedtls_ssl_config_free(&g_ssl_conf);
	mbedtls_x509_crt_free(&g_cacert);
	mbedtls_ctr_drbg_free(&g_ctr_drbg);
	mbedtls_entropy_free(&g_entropy);
	g_initialized = 0;

	printf("[%s] Cleaned up\n", TAG);
}

/* Simple URL parser: extract host, port, path from https://host[:port]/path */
static int parse_url(const char *url, char *host, int host_size,
		char *port, int port_size, char *path, int path_size)
{
	const char *p = url;

	/* Skip scheme */
	if (strncmp(p, "https://", 8) == 0)
		p += 8;
	else if (strncmp(p, "http://", 7) == 0)
		p += 7;
	else
		return -1;

	/* Extract host (and optional port) */
	const char *host_start = p;
	const char *host_end = NULL;
	const char *port_start = NULL;
	const char *path_start = NULL;

	while (*p && *p != '/' && *p != ':')
		p++;

	host_end = p;

	if (*p == ':') {
		p++;
		port_start = p;
		while (*p && *p != '/')
			p++;
	}

	path_start = (*p == '/') ? p : "/";

	/* Copy host */
	int hlen = host_end - host_start;
	if (hlen >= host_size)
		hlen = host_size - 1;
	memcpy(host, host_start, hlen);
	host[hlen] = '\0';

	/* Copy port */
	if (port_start) {
		int plen = p - port_start;
		if (plen >= port_size)
			plen = port_size - 1;
		memcpy(port, port_start, plen);
		port[plen] = '\0';
	} else {
		strncpy(port, "443", port_size - 1);
		port[port_size - 1] = '\0';
	}

	/* Copy path */
	strncpy(path, path_start, path_size - 1);
	path[path_size - 1] = '\0';

	return 0;
}

int https_post(const char *url, const char **headers,
               const char *body, int body_len, int *http_status)
{
	int ret;
	mbedtls_net_context server_fd;
	mbedtls_ssl_context ssl;
	char host[256], port[8], path[512];
	char request[2048];
	char response[RESPONSE_BUF_SIZE];
	int req_len;

	if (!g_initialized) {
		printf("[%s] Not initialized\n", TAG);
		return -1;
	}

	if (http_status)
		*http_status = 0;

	/* Parse URL */
	if (parse_url(url, host, sizeof(host), port, sizeof(port),
				path, sizeof(path)) < 0) {
		printf("[%s] Failed to parse URL: %s\n", TAG, url);
		return -1;
	}

	mbedtls_net_init(&server_fd);
	mbedtls_ssl_init(&ssl);

	/* Connect TCP */
	ret = mbedtls_net_connect(&server_fd, host, port, MBEDTLS_NET_PROTO_TCP);
	if (ret != 0) {
		printf("[%s] TCP connect to %s:%s failed: -0x%04x\n", TAG, host, port, -ret);
		goto cleanup;
	}

	/* Setup TLS */
	ret = mbedtls_ssl_setup(&ssl, &g_ssl_conf);
	if (ret != 0) {
		printf("[%s] ssl_setup failed: -0x%04x\n", TAG, -ret);
		goto cleanup;
	}

	ret = mbedtls_ssl_set_hostname(&ssl, host);
	if (ret != 0) {
		printf("[%s] ssl_set_hostname failed: -0x%04x\n", TAG, -ret);
		goto cleanup;
	}

	mbedtls_ssl_set_bio(&ssl, &server_fd,
			mbedtls_net_send, NULL, mbedtls_net_recv_timeout);

	/* TLS handshake */
	while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
		if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
			printf("[%s] TLS handshake failed: -0x%04x\n", TAG, -ret);
			goto cleanup;
		}
	}

	/* Build HTTP request */
	req_len = snprintf(request, sizeof(request),
		"POST %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"Content-Length: %d\r\n"
		"Connection: close\r\n",
		path, host, body_len);

	/* Append custom headers */
	if (headers) {
		int i;
		for (i = 0; headers[i] != NULL; i++) {
			req_len += snprintf(request + req_len, sizeof(request) - req_len,
				"%s\r\n", headers[i]);
		}
	}

	/* End of headers */
	req_len += snprintf(request + req_len, sizeof(request) - req_len, "\r\n");

	/* Send headers */
	ret = mbedtls_ssl_write(&ssl, (const unsigned char *)request, req_len);
	if (ret < 0) {
		printf("[%s] ssl_write (headers) failed: -0x%04x\n", TAG, -ret);
		goto cleanup;
	}

	/* Send body */
	if (body && body_len > 0) {
		int written = 0;
		while (written < body_len) {
			ret = mbedtls_ssl_write(&ssl,
					(const unsigned char *)body + written,
					body_len - written);
			if (ret < 0) {
				printf("[%s] ssl_write (body) failed: -0x%04x\n", TAG, -ret);
				goto cleanup;
			}
			written += ret;
		}
	}

	/* Read response (just need the status line) */
	memset(response, 0, sizeof(response));
	ret = mbedtls_ssl_read(&ssl, (unsigned char *)response, sizeof(response) - 1);
	if (ret < 0) {
		printf("[%s] ssl_read failed: -0x%04x\n", TAG, -ret);
		goto cleanup;
	}

	/* Parse HTTP status code from "HTTP/1.x NNN ..." */
	int status = 0;
	if (ret > 12 && strncmp(response, "HTTP/1.", 7) == 0) {
		status = atoi(response + 9);
	}

	if (http_status)
		*http_status = status;

	/* Close TLS gracefully */
	mbedtls_ssl_close_notify(&ssl);
	mbedtls_ssl_free(&ssl);
	mbedtls_net_free(&server_fd);

	if (status >= 200 && status < 300)
		return 0;

	printf("[%s] HTTP %d from %s%s\n", TAG, status, host, path);
	return -1;

cleanup:
	mbedtls_ssl_free(&ssl);
	mbedtls_net_free(&server_fd);
	return -1;
}

/* Helper: get current time in microseconds */
static long long get_time_us(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (long long)tv.tv_sec * 1000000LL + tv.tv_usec;
}

void adaptive_rate_init(adaptive_rate_t *ar, int target_pct, int max_rate_kbps)
{
	memset(ar, 0, sizeof(*ar));
	ar->target_pct = (target_pct > 0 && target_pct <= 100) ? target_pct : 80;
	ar->max_rate_kbps = max_rate_kbps;
}

#define UPLOAD_CHUNK_SIZE 8192

/* Probe window: send the first 512KB at full speed to measure link capacity */
#define PROBE_BYTES (512 * 1024)

/* EMA smoothing: alpha = 3/10 (reacts to changes while filtering jitter) */
#define EMA_ALPHA_NUM 3
#define EMA_ALPHA_DEN 10

/* Minimum adaptive rate to prevent stalling */
#define MIN_RATE_KBPS 10

int https_post_file(const char *url, const char **headers,
                    const char *filepath, int rate_limit_kbps,
                    adaptive_rate_t *adaptive,
                    int *http_status)
{
	int ret;
	mbedtls_net_context server_fd;
	mbedtls_ssl_context ssl;
	char host[256], port[8], path[512];
	char request[2048];
	char response[RESPONSE_BUF_SIZE];
	int req_len;
	struct stat st;
	FILE *fp = NULL;

	if (!g_initialized) {
		printf("[%s] Not initialized\n", TAG);
		return -1;
	}

	if (http_status)
		*http_status = 0;

	/* Get file size */
	if (stat(filepath, &st) != 0) {
		printf("[%s] stat(%s) failed\n", TAG, filepath);
		return -1;
	}
	long file_size = (long)st.st_size;

	/* Parse URL */
	if (parse_url(url, host, sizeof(host), port, sizeof(port),
				path, sizeof(path)) < 0) {
		printf("[%s] Failed to parse URL: %s\n", TAG, url);
		return -1;
	}

	/* Open file */
	fp = fopen(filepath, "rb");
	if (!fp) {
		printf("[%s] fopen(%s) failed\n", TAG, filepath);
		return -1;
	}

	mbedtls_net_init(&server_fd);
	mbedtls_ssl_init(&ssl);

	/* Connect TCP */
	ret = mbedtls_net_connect(&server_fd, host, port, MBEDTLS_NET_PROTO_TCP);
	if (ret != 0) {
		printf("[%s] TCP connect to %s:%s failed: -0x%04x\n", TAG, host, port, -ret);
		goto cleanup_file;
	}

	/* Setup TLS */
	ret = mbedtls_ssl_setup(&ssl, &g_ssl_conf);
	if (ret != 0) {
		printf("[%s] ssl_setup failed: -0x%04x\n", TAG, -ret);
		goto cleanup_file;
	}

	mbedtls_ssl_set_hostname(&ssl, host);
	mbedtls_ssl_set_bio(&ssl, &server_fd,
			mbedtls_net_send, NULL, mbedtls_net_recv_timeout);

	/* TLS handshake */
	while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
		if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
			printf("[%s] TLS handshake failed: -0x%04x\n", TAG, -ret);
			goto cleanup_file;
		}
	}

	/* Build HTTP request */
	req_len = snprintf(request, sizeof(request),
		"POST %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"Content-Length: %ld\r\n"
		"Connection: close\r\n",
		path, host, file_size);

	/* Append custom headers */
	if (headers) {
		int i;
		for (i = 0; headers[i] != NULL; i++) {
			req_len += snprintf(request + req_len, sizeof(request) - req_len,
				"%s\r\n", headers[i]);
		}
	}
	req_len += snprintf(request + req_len, sizeof(request) - req_len, "\r\n");

	/* Send headers */
	ret = mbedtls_ssl_write(&ssl, (const unsigned char *)request, req_len);
	if (ret < 0) {
		printf("[%s] ssl_write (headers) failed: -0x%04x\n", TAG, -ret);
		goto cleanup_file;
	}

	/* Stream file with rate limiting */
	{
		unsigned char buf[UPLOAD_CHUNK_SIZE];
		long bytes_sent_total = 0;
		long long start_us = get_time_us();
		size_t n;

		/* Adaptive rate state for this upload */
		int probe_done = 0;
		long long probe_write_us = 0;   /* Wall-clock time of writes during probe */
		long probe_bytes = 0;
		long bytes_since_probe = 0;     /* Bytes sent after probe completed */
		long long rate_limit_start_us = 0;
		int effective_rate = 0;         /* Active rate limit in KB/s */

		/* If adaptive mode with prior state, use existing rate for the probe
		 * window too (don't fully saturate on subsequent uploads). For the
		 * very first upload (no prior data), probe at full speed. */
		if (adaptive && adaptive->initialized) {
			/* Re-probe but at 2x current rate to detect increased capacity
			 * without fully saturating the link */
			effective_rate = adaptive->current_rate_kbps * 2;
			if (adaptive->max_rate_kbps > 0 && effective_rate > adaptive->max_rate_kbps)
				effective_rate = adaptive->max_rate_kbps;
		}

		while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
			long long write_start = get_time_us();

			size_t written = 0;
			while (written < n) {
				ret = mbedtls_ssl_write(&ssl, buf + written, n - written);
				if (ret < 0) {
					printf("[%s] ssl_write (file) failed at %ld/%ld: -0x%04x\n",
							TAG, bytes_sent_total, file_size, -ret);
					goto cleanup_file;
				}
				written += ret;
			}

			long long write_end = get_time_us();
			bytes_sent_total += n;

			if (adaptive) {
				if (!probe_done) {
					/* Probe phase: measure raw write throughput */
					probe_write_us += (write_end - write_start);
					probe_bytes += n;

					/* During re-probe with prior state, apply gentle rate limit */
					if (effective_rate > 0) {
						long long expected = (probe_bytes * 1000000LL) /
							((long long)effective_rate * 1024);
						long long elapsed = get_time_us() - start_us;
						if (expected > elapsed)
							usleep((unsigned int)(expected - elapsed));
					}

					if (probe_bytes >= PROBE_BYTES || probe_bytes >= file_size) {
						/* Probe complete */
						long long raw_bps = 0;
						if (probe_write_us > 0)
							raw_bps = (probe_bytes * 1000000LL) / probe_write_us;

						/* Update EMA */
						if (!adaptive->initialized) {
							adaptive->ema_bps = raw_bps;
							adaptive->initialized = 1;
						} else {
							adaptive->ema_bps =
								(EMA_ALPHA_NUM * raw_bps +
								 (EMA_ALPHA_DEN - EMA_ALPHA_NUM) * adaptive->ema_bps)
								/ EMA_ALPHA_DEN;
						}

						/* Compute adaptive rate = target_pct% of smoothed throughput */
						int rate = (int)((adaptive->ema_bps * adaptive->target_pct)
								/ 100 / 1024);
						if (adaptive->max_rate_kbps > 0 && rate > adaptive->max_rate_kbps)
							rate = adaptive->max_rate_kbps;
						if (rate < MIN_RATE_KBPS)
							rate = MIN_RATE_KBPS;

						adaptive->current_rate_kbps = rate;
						effective_rate = rate;
						probe_done = 1;
						rate_limit_start_us = get_time_us();
						bytes_since_probe = 0;

						printf("[%s] Adaptive: probe=%ldKB in %lldms raw=%lldKB/s "
								"ema=%lldKB/s target=%d%% -> rate=%dKB/s\n",
								TAG, probe_bytes / 1024,
								probe_write_us / 1000,
								raw_bps / 1024,
								adaptive->ema_bps / 1024,
								adaptive->target_pct, rate);
					}
				} else {
					/* Rate-limited phase */
					bytes_since_probe += n;
					long long expected_us = (bytes_since_probe * 1000000LL) /
						((long long)effective_rate * 1024);
					long long elapsed_us = get_time_us() - rate_limit_start_us;
					if (expected_us > elapsed_us)
						usleep((unsigned int)(expected_us - elapsed_us));
				}
			} else if (rate_limit_kbps > 0) {
				/* Static rate limiting (original behavior) */
				long long expected_us = (bytes_sent_total * 1000000LL) /
					((long long)rate_limit_kbps * 1024);
				long long elapsed_us = get_time_us() - start_us;
				if (expected_us > elapsed_us)
					usleep((unsigned int)(expected_us - elapsed_us));
			}
		}

		printf("[%s] Sent %ld bytes from %s\n", TAG, bytes_sent_total, filepath);
	}

	fclose(fp);
	fp = NULL;

	/* Read response */
	memset(response, 0, sizeof(response));
	ret = mbedtls_ssl_read(&ssl, (unsigned char *)response, sizeof(response) - 1);
	if (ret < 0) {
		printf("[%s] ssl_read failed: -0x%04x\n", TAG, -ret);
		goto cleanup_net;
	}

	/* Parse HTTP status */
	int status = 0;
	if (ret > 12 && strncmp(response, "HTTP/1.", 7) == 0)
		status = atoi(response + 9);

	if (http_status)
		*http_status = status;

	mbedtls_ssl_close_notify(&ssl);
	mbedtls_ssl_free(&ssl);
	mbedtls_net_free(&server_fd);

	if (status >= 200 && status < 300)
		return 0;

	printf("[%s] HTTP %d from %s%s\n", TAG, status, host, path);
	return -1;

cleanup_file:
	if (fp)
		fclose(fp);
cleanup_net:
	mbedtls_ssl_free(&ssl);
	mbedtls_net_free(&server_fd);
	return -1;
}
