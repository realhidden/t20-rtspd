#ifndef __HTTPS_CLIENT_H__
#define __HTTPS_CLIENT_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Adaptive rate limiter state.
 * Measures actual throughput during a probe window at the start of each upload,
 * then rate-limits to target_pct% of observed capacity for the remainder.
 * State persists across uploads so it converges quickly. */
typedef struct {
	int target_pct;           /* Target bandwidth utilization (e.g. 80 = use 80%) */
	int max_rate_kbps;        /* Hard upper cap in KB/s (0 = no cap) */
	/* --- internal state (zero-initialize before first use) --- */
	long long ema_bps;        /* Smoothed raw write throughput (bytes/sec) */
	int current_rate_kbps;    /* Current computed rate limit */
	int initialized;          /* Non-zero after first probe completes */
} adaptive_rate_t;

/* Initialize adaptive rate state. Call once before first upload. */
void adaptive_rate_init(adaptive_rate_t *ar, int target_pct, int max_rate_kbps);

/* One-time init/cleanup for mbedtls (call once at startup/shutdown).
 * https_client_init() is idempotent — safe to call multiple times. */
int https_client_init(void);
void https_client_cleanup(void);

/* Perform HTTPS POST.
 * url: full https:// URL
 * headers: NULL-terminated array of "Header: value" strings
 * body/body_len: POST payload
 * http_status: if non-NULL, receives the HTTP status code
 * Returns 0 on success (HTTP 2xx), -1 on error. */
int https_post(const char *url, const char **headers,
               const char *body, int body_len, int *http_status);

/* Upload a file via HTTPS POST, streaming from disk with optional rate limit.
 * filepath: path to the file to upload
 * rate_limit_kbps: KB/s limit (0 = unlimited), used when adaptive is NULL
 * adaptive: if non-NULL, use adaptive rate limiting (rate_limit_kbps is ignored)
 * Returns 0 on success (HTTP 2xx), -1 on error. */
int https_post_file(const char *url, const char **headers,
                    const char *filepath, int rate_limit_kbps,
                    adaptive_rate_t *adaptive,
                    int *http_status);

/* Upload a chunk (portion) of a file via HTTPS POST.
 * filepath: path to the file
 * offset: byte offset to start reading from
 * chunk_size: number of bytes to upload (0 = rest of file)
 * Returns 0 on success (HTTP 2xx), -1 on error. */
int https_post_chunk(const char *url, const char **headers,
                     const char *filepath, long offset, long chunk_size,
                     int *http_status);

#ifdef __cplusplus
}
#endif

#endif /* __HTTPS_CLIENT_H__ */
