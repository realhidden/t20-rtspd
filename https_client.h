#ifndef __HTTPS_CLIENT_H__
#define __HTTPS_CLIENT_H__

#ifdef __cplusplus
extern "C" {
#endif

/* One-time init/cleanup for mbedtls (call once at startup/shutdown) */
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

#ifdef __cplusplus
}
#endif

#endif /* __HTTPS_CLIENT_H__ */
