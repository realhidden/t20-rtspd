/*
 * http_server.c — mini HTTP server for Home Assistant integration
 *
 * Serves:
 *   GET /snapshot — captures a JPEG frame from the IMP encoder
 *   GET /status   — returns JSON camera status
 *
 * Runs in a dedicated thread. Minimal overhead — only active when
 * someone (e.g. Home Assistant) requests a snapshot.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#include "http_server.h"

#define TAG "http_server"
#define SNAP_PATH "/tmp/camera_snapshot.jpg"

static volatile int g_running = 0;
static pthread_t g_thread;
static int g_port = 8080;

/* Camera info callback — set by main to provide status */
static const char *(*g_get_camera_name)(void) = NULL;
static int (*g_get_uptime)(void) = NULL;

void http_server_set_callbacks(const char *(*getName)(void), int (*getUptime)(void)) {
    g_get_camera_name = getName;
    g_get_uptime = getUptime;
}

/* Capture a JPEG snapshot using IMP JPEG encoder */
static int capture_snapshot(void) {
    /* Use the existing sample_do_get_jpeg_snap from imp-common.c
     * which writes to SNAP_FILE_PATH_PREFIX/snap-*.jpg
     * We'll use a fixed path instead. */

    /* Simple approach: trigger the H264 encoder and grab a frame,
     * then the caller can serve whatever is in the snap directory.
     * For now, use the IMP snapshot API directly. */
    extern int sample_do_get_jpeg_snap(void);
    return sample_do_get_jpeg_snap();
}

/* Find the latest snapshot file */
static char *find_latest_snapshot(void) {
    static char snapPath[256];
    snapPath[0] = '\0';

    /* Check common snapshot locations */
    const char *dirs[] = {
        "/tmp",
        "/system/sdcard/snap",
        "/system/sdDCIM/Recording",
        NULL
    };

    time_t newest = 0;
    for (int d = 0; dirs[d]; d++) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "ls -t %s/snap-*.jpg 2>/dev/null | head -1", dirs[d]);
        FILE *pipe = popen(cmd, "r");
        if (!pipe) continue;

        char buf[256];
        if (fgets(buf, sizeof(buf), pipe)) {
            /* Remove trailing newline */
            buf[strcspn(buf, "\n")] = '\0';

            struct stat st;
            if (stat(buf, &st) == 0 && st.st_mtime > newest) {
                newest = st.st_mtime;
                strncpy(snapPath, buf, sizeof(snapPath) - 1);
            }
        }
        pclose(pipe);
    }

    return snapPath[0] ? snapPath : NULL;
}

/* Handle a single HTTP request */
static void handle_request(int client_fd) {
    char buf[1024];
    memset(buf, 0, sizeof(buf));

    int n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        close(client_fd);
        return;
    }

    /* Parse method and path */
    char method[16] = "", path[256] = "";
    sscanf(buf, "%15s %255s", method, path);

    if (strcmp(method, "GET") != 0) {
        const char *resp = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n";
        send(client_fd, resp, strlen(resp), 0);
        close(client_fd);
        return;
    }

    if (strcmp(path, "/snapshot") == 0) {
        /* Capture a fresh JPEG snapshot */
        int ret = capture_snapshot();
        if (ret < 0) {
            const char *resp = "HTTP/1.1 503 Snapshot Failed\r\nContent-Length: 0\r\n\r\n";
            send(client_fd, resp, strlen(resp), 0);
            close(client_fd);
            return;
        }

        /* Find the latest snapshot file */
        char *snapFile = find_latest_snapshot();
        if (!snapFile || access(snapFile, R_OK) != 0) {
            const char *resp = "HTTP/1.1 404 No Snapshot\r\nContent-Length: 0\r\n\r\n";
            send(client_fd, resp, strlen(resp), 0);
            close(client_fd);
            return;
        }

        /* Read the JPEG file */
        struct stat st;
        stat(snapFile, &st);
        int fileSize = st.st_size;

        /* Send HTTP response with JPEG */
        char header[256];
        int hlen = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %d\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "\r\n", fileSize);
        send(client_fd, header, hlen, 0);

        /* Send file data */
        int fd = open(snapFile, O_RDONLY);
        if (fd >= 0) {
            char fileBuf[4096];
            int remaining = fileSize;
            while (remaining > 0) {
                int toRead = remaining > (int)sizeof(fileBuf) ? (int)sizeof(fileBuf) : remaining;
                int read = read(fd, fileBuf, toRead);
                if (read <= 0) break;
                send(client_fd, fileBuf, read, 0);
                remaining -= read;
            }
            close(fd);
        }

        /* Clean up the snapshot file */
        unlink(snapFile);

    } else if (strcmp(path, "/status") == 0) {
        /* Return camera status as JSON */
        const char *name = g_get_camera_name ? g_get_camera_name() : "unknown";
        int uptime = g_get_uptime ? g_get_uptime() : 0;

        char body[512];
        int blen = snprintf(body, sizeof(body),
            "{\"name\":\"%s\",\"uptime\":%d,\"status\":\"online\"}",
            name, uptime);

        char header[256];
        int hlen = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n", blen);
        send(client_fd, header, hlen, 0);
        send(client_fd, body, blen, 0);

    } else {
        const char *resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        send(client_fd, resp, strlen(resp), 0);
    }

    close(client_fd);
}

/* HTTP server thread */
static void *http_server_thread(void *arg) {
    (void)arg;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        printf("[%s] socket() failed: %s\n", TAG, strerror(errno));
        return NULL;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(g_port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("[%s] bind() failed: %s\n", TAG, strerror(errno));
        close(server_fd);
        return NULL;
    }

    if (listen(server_fd, 4) < 0) {
        printf("[%s] listen() failed: %s\n", TAG, strerror(errno));
        close(server_fd);
        return NULL;
    }

    printf("[%s] HTTP server listening on port %d\n", TAG, g_port);

    while (g_running) {
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        int client_fd = accept(server_fd, (struct sockaddr *)&clientAddr, &clientLen);
        if (client_fd < 0) {
            if (g_running) printf("[%s] accept() failed: %s\n", TAG, strerror(errno));
            continue;
        }
        handle_request(client_fd);
    }

    close(server_fd);
    printf("[%s] HTTP server stopped\n", TAG);
    return NULL;
}

int http_server_start(int port) {
    g_port = port;
    g_running = 1;

    int ret = pthread_create(&g_thread, NULL, http_server_thread, NULL);
    if (ret != 0) {
        printf("[%s] pthread_create failed: %d\n", TAG, ret);
        g_running = 0;
        return -1;
    }

    return 0;
}

void http_server_stop(void) {
    if (!g_running) return;
    g_running = 0;
    /* The thread will exit when accept() fails after close() */
    pthread_join(g_thread, NULL);
}
