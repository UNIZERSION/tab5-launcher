/* HTTPS for OTA: a pull stream (open, read, close) over esp_http_client with the launcher's root
   CA bundle (certs/ota_roots.pem). Follows up to 5 redirects, also to another host (GitHub release
   downloads go to *.githubusercontent.com), asks for byte ranges, stops on a cancel flag, and names
   the failure (no Wi-Fi, DNS, connection, TLS, HTTP status, time-out) for the error dialogs.
   Blocking: call from a worker task, never from the LVGL task. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define L5_NET_CANCELLED ESP_ERR_INVALID_STATE /* returned once the cancel flag is set */

typedef enum {
    L5_NET_OK,
    L5_NET_NO_WIFI,  /* the launcher has no Wi-Fi connection */
    L5_NET_DNS,      /* the host name did not resolve */
    L5_NET_CONNECT,  /* no TCP connection */
    L5_NET_TLS,      /* TLS handshake or certificate check failed */
    L5_NET_HTTP,     /* the server answered with a non-2xx status */
    L5_NET_TIMEOUT,  /* no data for 15 s */
    L5_NET_DROPPED,  /* the connection ended before all data arrived */
    L5_NET_CANCEL,   /* the user cancelled */
    L5_NET_OTHER,
} l5_net_fail_t;

typedef struct {
    l5_net_fail_t fail;
    int status;          /* HTTP status of the last response, 0 when none */
    esp_err_t err;
    char text[112];      /* one sentence for the user */
} l5_net_err_t;

typedef struct l5_net_stream l5_net_stream_t;

/* GET `url`; with len > 0 only bytes offset .. offset+len-1, with len == 0 from offset to the end
   (Range; a server that ignores Range is handled by skipping). *length is the body length to expect
   (len, Content-Length, or -1), *total the size of the whole resource when known (Content-Range /
   Content-Length), else -1. */
esp_err_t l5_net_open(const char *url, uint32_t offset, uint32_t len, volatile bool *cancel, l5_net_stream_t **out,
                      int64_t *length, int64_t *total, l5_net_err_t *e);
/* > 0: bytes read (fewer than n only at the end); 0: complete; < 0: error (details in e) */
int l5_net_read(l5_net_stream_t *s, uint8_t *buf, size_t n, l5_net_err_t *e);
/* logs "GET host/path: N bytes in T ms (R KB/s)" and frees the stream */
void l5_net_close(l5_net_stream_t *s);

/* Whole body into a NUL-terminated PSRAM buffer of at most max bytes (catalog pages, manifests).
   Free with heap_caps_free. */
esp_err_t l5_net_fetch(const char *url, size_t max, volatile bool *cancel, char **out, size_t *len, l5_net_err_t *e);

/* percent-encoding for query values (search text, versions) */
void l5_net_urlencode(char *out, size_t len, const char *in);
