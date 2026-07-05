#ifndef __HTTP_SERVER_H__
#define __HTTP_SERVER_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Start a mini HTTP server on the given port.
 * Handles:
 *   GET /snapshot  — captures a JPEG frame and returns it
 *   GET /status    — returns JSON camera status
 * Returns 0 on success. Runs in its own thread. */
int http_server_start(int port);

/* Stop the HTTP server thread. */
void http_server_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* __HTTP_SERVER_H__ */
