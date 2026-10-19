#include "l5_net.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_tls_errors.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "l5_wifi.h"

static const char *TAG = "l5_net";

#define MAX_REDIRECTS 5
#define CONNECT_TIMEOUT_MS 15000 /* DNS, TCP, TLS handshake and the answer's headers */
#define READ_TIMEOUT_MS 30000    /* silence while the body comes */

struct l5_net_stream {
    esp_http_client_handle_t c;
    volatile bool *cancel;
    int status;
    int64_t length, total;
    uint32_t skip; /* a server that ignored Range: bytes to drop before offset */
    uint64_t got;
    int64_t t0;
    char where[120]; /* host/path?query for the log and the error sentences */
    char content_range[48];
};

static void set_err(l5_net_err_t *e, l5_net_fail_t fail, esp_err_t err, int status, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));
static void set_err(l5_net_err_t *e, l5_net_fail_t fail, esp_err_t err, int status, const char *fmt, ...)
{
    if (!e) return;
    e->fail = fail;
    e->err = err;
    e->status = status;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->text, sizeof(e->text), fmt, ap);
    va_end(ap);
}

static esp_err_t on_event(esp_http_client_event_t *ev)
{
    l5_net_stream_t *s = ev->user_data;
    if (ev->event_id == HTTP_EVENT_ON_HEADER && s && ev->header_key && ev->header_value &&
        !strcasecmp(ev->header_key, "Content-Range")) {
        strlcpy(s->content_range, ev->header_value, sizeof(s->content_range));
    }
    return ESP_OK;
}

static void where(char *out, size_t len, const char *url)
{
    const char *p = strstr(url, "://");
    strlcpy(out, p ? p + 3 : url, len);
}

/* the host part of where(): "m5burner-cdn.m5stack.com" */
static int host_len(const char *where)
{
    return (int)strcspn(where, "/?");
}

/* esp-tls tells the reason of a failed open: DNS, TCP, time-out or the TLS handshake */
static void open_failed(l5_net_stream_t *s, esp_err_t err, l5_net_err_t *e)
{
    int tls_code = 0, tls_flags = 0;
    esp_err_t tls = esp_http_client_get_and_clear_last_tls_error(s->c, &tls_code, &tls_flags);
    int h = host_len(s->where);
    if (tls == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME) {
        set_err(e, L5_NET_DNS, tls, 0, "%.*s did not resolve (DNS). Check the network.", h, s->where);
    } else if (tls == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT) {
        set_err(e, L5_NET_TIMEOUT, tls, 0, "%.*s did not answer within %d s.", h, s->where, CONNECT_TIMEOUT_MS / 1000);
    } else if (tls == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST) {
        set_err(e, L5_NET_CONNECT, tls, 0, "No connection to %.*s.", h, s->where);
    } else if (tls == ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED || tls_flags) {
        set_err(e, L5_NET_TLS, tls, 0, "Secure connection to %.*s failed (TLS -0x%04x, certificate flags 0x%x).", h,
                s->where, -tls_code, tls_flags);
    } else {
        set_err(e, L5_NET_CONNECT, err, 0, "Connection to %.*s failed (%s).", h, s->where, esp_err_to_name(err));
    }
}

static bool is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

esp_err_t l5_net_open(const char *url, uint32_t offset, uint32_t len, volatile bool *cancel, l5_net_stream_t **out,
                      int64_t *length, int64_t *total, l5_net_err_t *e)
{
    l5_net_err_t local = {0};
    if (!e) e = &local;
    *out = NULL;
    l5_wifi_status_t ws;
    l5_wifi_get_status(&ws);
    if (ws.state != L5_WIFI_CONNECTED) {
        set_err(e, L5_NET_NO_WIFI, ESP_ERR_INVALID_STATE, 0, "No Wi-Fi connection.");
        return ESP_ERR_INVALID_STATE;
    }
    l5_net_stream_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_DEFAULT);
    if (!s) return ESP_ERR_NO_MEM;
    s->cancel = cancel;
    s->t0 = esp_timer_get_time();
    where(s->where, sizeof(s->where), url);
    char ua[48];
    snprintf(ua, sizeof(ua), "Tab5-Launcher/%.16s", esp_app_get_description()->version);
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = on_event,
        .user_data = s,
        .timeout_ms = CONNECT_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach, /* certs/ota_roots.pem */
        .disable_auto_redirect = true,              /* followed below, so a stream can read the body */
        .user_agent = ua,
    };
    s->c = esp_http_client_init(&cfg);
    if (!s->c) {
        heap_caps_free(s);
        set_err(e, L5_NET_OTHER, ESP_ERR_NO_MEM, 0, "No memory for the HTTPS client.");
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(s->c, "Accept-Encoding", "identity");
    if (len || offset) { /* bytes=a-b, or a- up to the end */
        char range[48];
        if (len) snprintf(range, sizeof(range), "bytes=%lu-%lu", (unsigned long)offset, (unsigned long)(offset + len - 1));
        else snprintf(range, sizeof(range), "bytes=%lu-", (unsigned long)offset);
        esp_http_client_set_header(s->c, "Range", range);
    }
    esp_err_t err = ESP_OK;
    bool retried = false;
    for (int hop = 0;; hop++) {
        if (cancel && *cancel) {
            err = L5_NET_CANCELLED;
            set_err(e, L5_NET_CANCEL, err, 0, "Cancelled.");
            goto fail;
        }
        s->content_range[0] = 0;
        err = esp_http_client_open(s->c, 0);
        if (err != ESP_OK) {
            open_failed(s, err, e);
            if (!retried && (e->fail == L5_NET_TIMEOUT || e->fail == L5_NET_CONNECT)) { /* once, after a short pause */
                retried = true;
                ESP_LOGW(TAG, "GET %s: %s Trying once more.", s->where, e->text);
                esp_http_client_close(s->c);
                vTaskDelay(pdMS_TO_TICKS(500));
                hop--; /* not a redirect */
                continue;
            }
            goto fail;
        }
        s->length = esp_http_client_fetch_headers(s->c);
        s->status = esp_http_client_get_status_code(s->c);
        if (s->length < 0 && s->status == 0) {
            err = ESP_ERR_HTTP_FETCH_HEADER;
            set_err(e, L5_NET_DROPPED, err, 0, "The server closed the connection before answering.");
            goto fail;
        }
        if (!is_redirect(s->status)) break;
        if (hop == MAX_REDIRECTS || esp_http_client_set_redirection(s->c) != ESP_OK) {
            err = ESP_ERR_HTTP_MAX_REDIRECT;
            set_err(e, L5_NET_HTTP, err, s->status, "Too many redirects (HTTP %d).", s->status);
            goto fail;
        }
        esp_http_client_close(s->c);
        char to[256]; /* logs and error sentences name the host that answers in the end */
        if (esp_http_client_get_url(s->c, to, sizeof(to)) == ESP_OK) where(s->where, sizeof(s->where), to);
    }
    if (s->status < 200 || s->status > 299) {
        err = ESP_FAIL;
        set_err(e, L5_NET_HTTP, err, s->status, "%.*s answered HTTP %d%s.", host_len(s->where), s->where, s->status,
                s->status == 404 ? " (file not found)" : s->status == 416 ? " (range not available)" : "");
        goto fail;
    }
    esp_http_client_set_timeout_ms(s->c, READ_TIMEOUT_MS); /* connected: now only the body's silence counts */
    if (esp_http_client_is_chunked_response(s->c)) s->length = -1;
    s->total = s->status == 200 ? s->length : -1;
    if (s->content_range[0]) { /* "bytes a-b/total" */
        const char *slash = strchr(s->content_range, '/');
        if (slash && slash[1] != '*') s->total = strtoll(slash + 1, NULL, 10);
    }
    if ((len || offset) && s->status == 200) { /* Range ignored: the whole file comes; drop what precedes offset */
        s->skip = offset;
        if (s->length >= 0) s->length = s->length > (int64_t)offset ? s->length - offset : 0;
    }
    if (len && (s->length < 0 || s->length > (int64_t)len)) s->length = len;
    if (length) *length = s->length;
    if (total) *total = s->total;
    *out = s;
    return ESP_OK;
fail:
    if (e->fail != L5_NET_CANCEL) ESP_LOGW(TAG, "GET %s failed: %s", s->where, e->text);
    esp_http_client_cleanup(s->c);
    heap_caps_free(s);
    return err == ESP_OK ? ESP_FAIL : err;
}

int l5_net_read(l5_net_stream_t *s, uint8_t *buf, size_t n, l5_net_err_t *e)
{
    if (s->length >= 0 && s->got >= (uint64_t)s->length) return 0; /* a Range answer can carry more: stop */
    if (s->length >= 0 && s->got + n > (uint64_t)s->length) n = s->length - s->got;
    for (;;) {
        if (s->cancel && *s->cancel) {
            set_err(e, L5_NET_CANCEL, L5_NET_CANCELLED, s->status, "Cancelled.");
            return -1;
        }
        int r = esp_http_client_read(s->c, (char *)buf, (int)n);
        if (r == -ESP_ERR_HTTP_EAGAIN) {
            set_err(e, L5_NET_TIMEOUT, ESP_ERR_HTTP_READ_TIMEOUT, s->status,
                    "No data for %d s: the Wi-Fi link or the server stopped.", READ_TIMEOUT_MS / 1000);
            ESP_LOGW(TAG, "GET %s: %s", s->where, e ? e->text : "time-out");
            return -1;
        }
        if (r < 0) {
            set_err(e, L5_NET_DROPPED, ESP_FAIL, s->status, "The connection broke off after %llu bytes.",
                    (unsigned long long)s->got);
            ESP_LOGW(TAG, "GET %s: %s", s->where, e ? e->text : "dropped");
            return -1;
        }
        if (r == 0) {
            if (s->length >= 0 && s->got < (uint64_t)s->length) {
                set_err(e, L5_NET_DROPPED, ESP_ERR_HTTP_INCOMPLETE_DATA, s->status,
                        "The download stopped at %llu of %lld bytes.", (unsigned long long)s->got, (long long)s->length);
                ESP_LOGW(TAG, "GET %s: %s", s->where, e ? e->text : "short");
                return -1;
            }
            return 0;
        }
        if (s->skip) { /* still before the requested range */
            uint32_t drop = (uint32_t)r < s->skip ? (uint32_t)r : s->skip;
            s->skip -= drop;
            if (drop == (uint32_t)r) continue;
            memmove(buf, buf + drop, r - drop);
            r -= drop;
        }
        s->got += r;
        return r;
    }
}

void l5_net_close(l5_net_stream_t *s)
{
    if (!s) return;
    int64_t ms = (esp_timer_get_time() - s->t0) / 1000;
    ESP_LOGI(TAG, "GET %s: %llu bytes in %lld ms (%llu KB/s), HTTP %d", s->where, (unsigned long long)s->got, ms,
             ms > 0 ? (unsigned long long)(s->got * 1000 / 1024 / ms) : 0ULL, s->status);
    esp_http_client_close(s->c);
    esp_http_client_cleanup(s->c);
    heap_caps_free(s);
}

esp_err_t l5_net_fetch(const char *url, size_t max, volatile bool *cancel, char **out, size_t *len, l5_net_err_t *e)
{
    *out = NULL;
    *len = 0;
    l5_net_stream_t *s;
    int64_t length = -1;
    esp_err_t err = l5_net_open(url, 0, 0, cancel, &s, &length, NULL, e);
    if (err != ESP_OK) return err;
    if (length > (int64_t)max) {
        l5_net_close(s);
        set_err(e, L5_NET_OTHER, ESP_ERR_INVALID_SIZE, 200, "The answer is larger than %u KB.", (unsigned)(max / 1024));
        return ESP_ERR_INVALID_SIZE;
    }
    size_t cap = length > 0 ? (size_t)length + 1 : 8192;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    size_t used = 0;
    while (buf) {
        if (used + 1024 + 1 > cap) { /* chunked or longer than announced */
            size_t ncap = cap * 2 > max + 1 ? max + 1 : cap * 2;
            if (ncap <= cap) {
                err = ESP_ERR_INVALID_SIZE;
                set_err(e, L5_NET_OTHER, err, 200, "The answer is larger than %u KB.", (unsigned)(max / 1024));
                break;
            }
            char *nb = heap_caps_realloc(buf, ncap, MALLOC_CAP_SPIRAM);
            if (!nb) break;
            buf = nb;
            cap = ncap;
        }
        int r = l5_net_read(s, (uint8_t *)buf + used, cap - 1 - used, e);
        if (r < 0) {
            err = e && e->fail == L5_NET_CANCEL ? L5_NET_CANCELLED : ESP_FAIL;
            break;
        }
        if (r == 0) {
            err = ESP_OK;
            break;
        }
        used += r;
    }
    l5_net_close(s);
    if (!buf) {
        set_err(e, L5_NET_OTHER, ESP_ERR_NO_MEM, 0, "No memory for the answer.");
        return ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        heap_caps_free(buf);
        return err;
    }
    buf[used] = 0;
    *out = buf;
    *len = used;
    return ESP_OK;
}

void l5_net_urlencode(char *out, size_t len, const char *in)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; in && *in && o + 4 < len; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out[o++] = c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    if (len) out[o < len ? o : len - 1] = 0;
}
