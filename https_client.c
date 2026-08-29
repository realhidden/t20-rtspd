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
#define READ_TIMEOUT_MS 60000
#define RESPONSE_BUF_SIZE 1024

/* Global state (initialized once) */
static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_ctr_drbg;
static mbedtls_x509_crt g_cacert;
static mbedtls_ssl_config g_ssl_conf;
static int g_initialized = 0;
static int g_has_cacert = 0;

static void ka_drop(void);

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

	ka_drop();
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
	const char *sp = response;
	/* Find first space after HTTP version */
	while (*sp && *sp != ' ' && sp < response + ret)
		sp++;
	if (*sp == ' ')
		status = atoi(sp + 1);

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

void adaptive_rate_init(adaptive_rate_t *ar, int target_pct, int max_rate_kbps)
{
	memset(ar, 0, sizeof(*ar));
	ar->target_pct = (target_pct > 0 && target_pct <= 100) ? target_pct : 80;
	ar->max_rate_kbps = max_rate_kbps;
}

/* Case-insensitive substring search (strcasestr is GNU-only). */
static char *ci_strstr(const char *haystack, const char *needle)
{
	size_t nl = strlen(needle);
	if (nl == 0) return (char *)haystack;
	for (; *haystack; haystack++) {
		size_t i;
		for (i = 0; i < nl; i++) {
			char a = haystack[i], b = needle[i];
			if (a >= 'A' && a <= 'Z') a += 32;
			if (b >= 'A' && b <= 'Z') b += 32;
			if (a != b) break;
		}
		if (i == nl) return (char *)haystack;
	}
	return NULL;
}

/* File-to-network stream buffer size for chunked uploads */
#define UPLOAD_CHUNK_SIZE 8192

/* --- Persistent TLS connection for chunk uploads ---
 * Consecutive chunks of a file go to the same host ~1s apart; reusing one
 * TLS connection saves a full handshake (~0.5-1s of CPU on this SoC) per
 * 1MB chunk — the dominant load spike during uploads. Server-side idle
 * timeouts are handled by the retry-once-on-stale logic in the caller. */
static mbedtls_net_context g_ka_fd;
static mbedtls_ssl_context g_ka_ssl;
static int g_ka_valid = 0;
static char g_ka_host[256];
static char g_ka_port[8];

static void ka_drop(void)
{
	if (!g_ka_valid)
		return;
	mbedtls_ssl_close_notify(&g_ka_ssl);
	mbedtls_ssl_free(&g_ka_ssl);
	mbedtls_net_free(&g_ka_fd);
	g_ka_valid = 0;
}

/* Establish the persistent connection (must not already be valid). */
static int ka_connect(const char *host, const char *port)
{
	int ret;

	mbedtls_net_init(&g_ka_fd);
	mbedtls_ssl_init(&g_ka_ssl);

	ret = mbedtls_net_connect(&g_ka_fd, host, port, MBEDTLS_NET_PROTO_TCP);
	if (ret != 0) {
		printf("[%s] TCP connect to %s:%s failed: -0x%04x\n", TAG, host, port, -ret);
		goto fail;
	}

	ret = mbedtls_ssl_setup(&g_ka_ssl, &g_ssl_conf);
	if (ret != 0) {
		printf("[%s] ssl_setup failed: -0x%04x\n", TAG, -ret);
		goto fail;
	}

	mbedtls_ssl_set_hostname(&g_ka_ssl, host);
	mbedtls_ssl_set_bio(&g_ka_ssl, &g_ka_fd,
			mbedtls_net_send, NULL, mbedtls_net_recv_timeout);

	while ((ret = mbedtls_ssl_handshake(&g_ka_ssl)) != 0) {
		if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
			printf("[%s] TLS handshake failed: -0x%04x\n", TAG, -ret);
			goto fail;
		}
	}

	g_ka_valid = 1;
	strncpy(g_ka_host, host, sizeof(g_ka_host) - 1);
	g_ka_host[sizeof(g_ka_host) - 1] = '\0';
	strncpy(g_ka_port, port, sizeof(g_ka_port) - 1);
	g_ka_port[sizeof(g_ka_port) - 1] = '\0';
	return 0;

fail:
	mbedtls_ssl_free(&g_ka_ssl);
	mbedtls_net_free(&g_ka_fd);
	return -1;
}

/* Send one chunk request over the persistent connection and read the full
 * response (needed before the connection may be reused). Sets *http_status
 * and *conn_dead (server closed / unusable). Returns 0 on I/O success
 * (regardless of status code), -1 on connection error. */
static int ka_send_chunk(const char *path, const char *host, const char **headers,
		FILE *fp, long chunk_size, int *http_status, int *conn_dead)
{
	char request[2048];
	char response[RESPONSE_BUF_SIZE];
	int req_len, ret, i;

	*conn_dead = 1;  /* assume unusable until proven otherwise */

	req_len = snprintf(request, sizeof(request),
		"POST %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"Content-Length: %ld\r\n"
		"Connection: keep-alive\r\n",
		path, host, chunk_size);

	if (headers) {
		for (i = 0; headers[i] != NULL; i++) {
			req_len += snprintf(request + req_len, sizeof(request) - req_len,
				"%s\r\n", headers[i]);
		}
	}
	req_len += snprintf(request + req_len, sizeof(request) - req_len, "\r\n");

	/* Send headers */
	ret = mbedtls_ssl_write(&g_ka_ssl, (const unsigned char *)request, req_len);
	if (ret < 0) {
		printf("[%s] ssl_write (headers) failed: -0x%04x\n", TAG, -ret);
		return -1;
	}

	/* Stream chunk from file */
	{
		unsigned char buf[UPLOAD_CHUNK_SIZE];
		long bytes_sent = 0;

		while (bytes_sent < chunk_size) {
			size_t to_read = chunk_size - bytes_sent;
			if (to_read > UPLOAD_CHUNK_SIZE)
				to_read = UPLOAD_CHUNK_SIZE;

			size_t n = fread(buf, 1, to_read, fp);
			if (n == 0) break;

			size_t written = 0;
			while (written < n) {
				ret = mbedtls_ssl_write(&g_ka_ssl, buf + written, n - written);
				if (ret < 0) {
					printf("[%s] ssl_write (chunk) failed at %ld/%ld: -0x%04x\n",
							TAG, bytes_sent, chunk_size, -ret);
					return -1;
				}
				written += ret;
			}
			bytes_sent += n;
		}
	}

	/* Read the complete response so the connection can be reused */
	{
		int total = 0;
		int header_end = -1;
		int content_len = -1;
		int close_hdr = 0;

		memset(response, 0, sizeof(response));
		while (total < (int)sizeof(response) - 1) {
			ret = mbedtls_ssl_read(&g_ka_ssl,
					(unsigned char *)response + total, sizeof(response) - 1 - total);
			if (ret == 0)
				break;	/* peer closed */
			if (ret < 0) {
				if (total > 0 && header_end >= 0)
					break;  /* got headers; body truncated is fine for us */
				printf("[%s] ssl_read failed: -0x%04x\n", TAG, -ret);
				return -1;
			}
			total += ret;
			response[total] = '\0';

			if (header_end < 0) {
				char *he = strstr(response, "\r\n\r\n");
				if (he) {
					header_end = (he + 4) - response;
					/* Content-Length and Connection: close from headers */
					char *cl = ci_strstr(response, "content-length:");
					if (cl)
						content_len = atoi(cl + 15);
					char *cn = ci_strstr(response, "connection:");
					if (cn && strncasecmp(cn + 11, " close", 6) == 0)
						close_hdr = 1;
				}
			}
			if (header_end >= 0 && content_len >= 0 &&
					total >= header_end + content_len)
				break;  /* full response received */
			if (header_end >= 0 && content_len < 0 && ret == 0)
				break;
		}

		/* Parse status from the first line */
		{
			const char *sp = response;
			while (*sp && *sp != ' ' && sp < response + total)
				sp++;
			if (*sp == ' ')
				*http_status = atoi(sp + 1);
		}

		if (!close_hdr && header_end >= 0)
			*conn_dead = 0;  /* connection stays usable */
	}

	return 0;
}

int https_post_chunk(const char *url, const char **headers,
                     const char *filepath, long offset, long chunk_size,
                     int *http_status)
{
	char host[256], port[8], path[512];
	struct stat st;
	int attempt;

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

	/* Clamp chunk_size to remaining bytes */
	if (chunk_size <= 0 || offset + chunk_size > (long)st.st_size)
		chunk_size = st.st_size - offset;

	/* Parse URL */
	if (parse_url(url, host, sizeof(host), port, sizeof(port),
				path, sizeof(path)) < 0) {
		printf("[%s] Failed to parse URL: %s\n", TAG, url);
		return -1;
	}

	for (attempt = 0; attempt < 2; attempt++) {
		FILE *fp = fopen(filepath, "rb");
		if (!fp) {
			printf("[%s] fopen(%s) failed\n", TAG, filepath);
			return -1;
		}
		if (fseek(fp, offset, SEEK_SET) != 0) {
			printf("[%s] fseek(%ld) failed\n", TAG, offset);
			fclose(fp);
			return -1;
		}

		int cached = g_ka_valid && strcmp(g_ka_host, host) == 0
			&& strcmp(g_ka_port, port) == 0;
		if (g_ka_valid && !cached)
			ka_drop();
		if (!cached && ka_connect(host, port) < 0) {
			fclose(fp);
			return -1;	/* fresh connection failed — network trouble */
		}

		int conn_dead = 1;
		int ret = ka_send_chunk(path, host, headers, fp, chunk_size,
				http_status, &conn_dead);
		fclose(fp);

		if (ret == 0) {
			if (conn_dead)
				ka_drop();
			if (*http_status >= 200 && *http_status < 300)
				return 0;
			printf("[%s] HTTP %d from %s%s\n", TAG, *http_status, host, path);
			return -1;
		}

		/* Connection-level failure. A stale cached connection is expected
		 * (server-side idle timeouts); drop it and retry once on a fresh
		 * one. A failure on an already-fresh connection means the network
		 * is down — give up. Re-sent chunks are deduplicated server-side. */
		ka_drop();
		if (!cached) {
			*http_status = 0;
			return -1;
		}
	}

	return -1;
}
