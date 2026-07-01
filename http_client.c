#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include "mbedtls/build_info.h"

#include "http_client.h"
#include "ca_bundle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>

#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/debug.h"

#define HTTPC_MAX_BODY      (4u * 1024u * 1024u)
#define HTTPC_READ_CHUNK    4096
#define HTTPC_TIMEOUT_MS    15000
#define HTTPC_MIN_EPOCH     1704067200L  /* 2024-01-01 UTC */

static char g_last_err[256];

static mbedtls_x509_crt  g_cacert;
static pthread_once_t     g_cacert_once = PTHREAD_ONCE_INIT;
static void init_cacert(void) {
    mbedtls_x509_crt_init(&g_cacert);
    mbedtls_x509_crt_parse(&g_cacert, ca_bundle_pem, ca_bundle_pem_len);
}

static void set_err(const char *prefix, int mbed_ret) {
    if (mbed_ret != 0) {
        char tmp[160];
        mbedtls_strerror(mbed_ret, tmp, sizeof tmp);
        snprintf(g_last_err, sizeof g_last_err, "%s: %s", prefix, tmp);
    } else {
        snprintf(g_last_err, sizeof g_last_err, "%s", prefix);
    }
}

const char *http_client_strerror(void) {
    return g_last_err[0] ? g_last_err : "no error";
}

/* Returns 0=HTTP, 1=HTTPS, negative=error.  Sets default port. */
static int parse_url(const char *url,
                     char *host, size_t hsz,
                     char *port, size_t psz,
                     char *path, size_t pasz) {
    const char *p = url;
    int tls;
    if (strncmp(p, "https://", 8) == 0) {
        tls = 1; p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        tls = 0; p += 7;
    } else {
        return HTTPC_ERR_URL;
    }

    const char *host_start = p;
    const char *host_end = p;
    while (*host_end && *host_end != ':' && *host_end != '/') host_end++;
    size_t host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || host_len >= hsz) return HTTPC_ERR_URL;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';

    if (*host_end == ':') {
        const char *port_start = host_end + 1;
        const char *port_end = port_start;
        while (*port_end && *port_end != '/') port_end++;
        size_t port_len = (size_t)(port_end - port_start);
        if (port_len == 0 || port_len >= psz) return HTTPC_ERR_URL;
        memcpy(port, port_start, port_len);
        port[port_len] = '\0';
        p = port_end;
    } else {
        snprintf(port, psz, tls ? "443" : "80");
        p = host_end;
    }

    if (*p == '\0') {
        snprintf(path, pasz, "/");
    } else {
        size_t path_len = strlen(p);
        if (path_len >= pasz) return HTTPC_ERR_URL;
        memcpy(path, p, path_len + 1);
    }

    return tls;
}

static int buffer_append(struct MemoryStruct *m, const void *data, size_t n) {
    if (m->size + n + 1 < m->size) return HTTPC_ERR_OOM;
    if (m->size + n > HTTPC_MAX_BODY) return HTTPC_ERR_HTTP;
    char *p = realloc(m->memory, m->size + n + 1);
    if (!p) return HTTPC_ERR_OOM;
    m->memory = p;
    memcpy(m->memory + m->size, data, n);
    m->size += n;
    m->memory[m->size] = '\0';
    return HTTPC_OK;
}

static int decode_chunked(const char *body, size_t body_len, struct MemoryStruct *out) {
    const char *p   = body;
    const char *end = body + body_len;

    while (p < end) {
        /* chunk-size must start with a hex digit — reject leading sign/space
         * that strtoul() would otherwise silently tolerate. */
        if (!isxdigit((unsigned char)*p)) return HTTPC_ERR_HTTP;
        char *endptr = NULL;
        unsigned long chunk_size = strtoul(p, &endptr, 16);
        if (endptr == p) return HTTPC_ERR_HTTP;

        /* skip optional chunk-extension and CRLF */
        while (endptr < end && *endptr != '\n') endptr++;
        if (endptr >= end) return HTTPC_ERR_HTTP;
        endptr++;   /* skip '\n' */

        if (chunk_size == 0) break;   /* last-chunk */
        if (chunk_size > (size_t)(end - endptr)) return HTTPC_ERR_HTTP;

        int ar = buffer_append(out, endptr, chunk_size);
        if (ar != HTTPC_OK) return ar;

        p = endptr + chunk_size;
        if (p < end && *p == '\r') p++;
        if (p < end && *p == '\n') p++;
    }
    return HTTPC_OK;
}


static int contains_ci(const char *hay, size_t hay_len, const char *needle) {
    size_t n = strlen(needle);
    if (n > hay_len) return 0;
    for (size_t i = 0; i + n <= hay_len; i++) {
        size_t j = 0;
        while (j < n) {
            char a = hay[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            j++;
        }
        if (j == n) return 1;
    }
    return 0;
}

/* Scan the header block for a Content-Length line (case-insensitive, matched
 * only at buffer start or just after '\n' so a value inside another header
 * can't match). Returns 1 and sets *out on success, 0 if absent/unparseable. */
static int find_content_length(const char *hdr, size_t hdr_len, size_t *out) {
    static const char key[] = "content-length:";
    const size_t klen = sizeof(key) - 1;
    for (size_t i = 0; i < hdr_len; i++) {
        if (i > 0 && hdr[i - 1] != '\n') continue;
        if (i + klen > hdr_len) break;
        size_t j = 0;
        while (j < klen) {
            char a = hdr[i + j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (a != key[j]) break;
            j++;
        }
        if (j < klen) continue;
        const char *val = hdr + i + klen;
        const char *end = hdr + hdr_len;
        while (val < end && (*val == ' ' || *val == '\t')) val++;
        if (val >= end) continue;
        char *ep;
        unsigned long v = strtoul(val, &ep, 10);
        if (ep == val) continue;
        *out = (size_t)v;
        return 1;
    }
    return 0;
}

int https_get(const char *url, const char *user_agent, struct MemoryStruct *out) {
    int rc = HTTPC_ERR_INTERNAL;
    int ret;

    g_last_err[0] = '\0';

    if (!url || !out) {
        set_err("invalid argument", 0);
        return HTTPC_ERR_INTERNAL;
    }

    /* Skip clock check for plain HTTP (geolocation only, no TLS cert validation) */
    char host[256], port[8], path[1024];
    int use_tls = parse_url(url, host, sizeof host, port, sizeof port, path, sizeof path);
    if (use_tls < 0) {
        set_err("URL parse failed", 0);
        return HTTPC_ERR_URL;
    }

    if (use_tls) {
        time_t now = time(NULL);
        if (now < HTTPC_MIN_EPOCH) {
            set_err("clock not set (RTC drift)", 0);
            return HTTPC_ERR_CLOCK;
        }
    }

    mbedtls_net_context     net;
    mbedtls_ssl_context     ssl;
    mbedtls_ssl_config      conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;

    mbedtls_net_init(&net);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);

    ret = mbedtls_net_connect(&net, host, port, MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) {
        set_err("connect", ret);
        rc = (ret == MBEDTLS_ERR_NET_UNKNOWN_HOST) ? HTTPC_ERR_DNS : HTTPC_ERR_CONNECT;
        goto cleanup;
    }

    if (use_tls) {
        static const char *pers = "kobo_weather";
        ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                    (const unsigned char *)pers, strlen(pers));
        if (ret != 0) { set_err("ctr_drbg_seed", ret); rc = HTTPC_ERR_INTERNAL; goto cleanup; }

        pthread_once(&g_cacert_once, init_cacert);

        ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT);
        if (ret != 0) { set_err("ssl_config_defaults", ret); rc = HTTPC_ERR_TLS; goto cleanup; }

        mbedtls_ssl_conf_dbg(&conf, NULL, NULL);
#ifdef MBEDTLS_DEBUG_C
        mbedtls_debug_set_threshold(0);
#endif

        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&conf, &g_cacert, NULL);
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
        mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_read_timeout(&conf, HTTPC_TIMEOUT_MS);

        ret = mbedtls_ssl_setup(&ssl, &conf);
        if (ret != 0) { set_err("ssl_setup", ret); rc = HTTPC_ERR_TLS; goto cleanup; }

        ret = mbedtls_ssl_set_hostname(&ssl, host);
        if (ret != 0) { set_err("set_hostname", ret); rc = HTTPC_ERR_TLS; goto cleanup; }

        mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, mbedtls_net_recv,
                            mbedtls_net_recv_timeout);

        while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                set_err("handshake", ret);
                rc = HTTPC_ERR_TLS;
                goto cleanup;
            }
        }

        uint32_t vflags = mbedtls_ssl_get_verify_result(&ssl);
        if (vflags != 0) {
            char info[160];
            mbedtls_x509_crt_verify_info(info, sizeof info, "  ", vflags);
            snprintf(g_last_err, sizeof g_last_err, "cert verify failed: %s", info);
            rc = HTTPC_ERR_TLS;
            goto cleanup;
        }
    }

    char req[1024];
    int req_len = snprintf(req, sizeof req,
                           "GET %s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "User-Agent: %s\r\n"
                           "Accept: application/json\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           path, host, user_agent ? user_agent : "kobo_weather/1.0");
    if (req_len <= 0 || req_len >= (int)sizeof req) {
        set_err("request too large", 0);
        rc = HTTPC_ERR_INTERNAL;
        goto cleanup;
    }

    /* Send request */
    size_t written = 0;
    while (written < (size_t)req_len) {
        if (use_tls) {
            ret = mbedtls_ssl_write(&ssl, (const unsigned char *)req + written,
                                    (size_t)req_len - written);
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        } else {
            ret = mbedtls_net_send(&net, (const unsigned char *)req + written,
                                   (size_t)req_len - written);
            if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        }
        if (ret > 0) { written += (size_t)ret; continue; }
        set_err(use_tls ? "ssl_write" : "net_send", ret);
        rc = HTTPC_ERR_IO;
        goto cleanup;
    }

    /* Read the whole body up to HTTPC_MAX_BODY (4 MB; buffer_append rejects
     * beyond it). No early abort on an oversized response without Content-Length. */
    struct MemoryStruct raw = { malloc(1), 0 };
    if (!raw.memory) { set_err("oom raw", 0); rc = HTTPC_ERR_OOM; goto cleanup; }
    raw.memory[0] = '\0';

    unsigned char buf[HTTPC_READ_CHUNK];
    for (;;) {
        if (use_tls) {
            ret = mbedtls_ssl_read(&ssl, buf, sizeof buf);
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0) break;
        } else {
            ret = mbedtls_net_recv(&net, buf, sizeof buf);
            if (ret == MBEDTLS_ERR_SSL_WANT_READ) continue;
            if (ret == 0) break;
        }
        if (ret < 0) {
            set_err(use_tls ? "ssl_read" : "net_recv", ret);
            rc = HTTPC_ERR_IO;
            free(raw.memory);
            goto cleanup;
        }
        int ar = buffer_append(&raw, buf, (size_t)ret);
        if (ar != HTTPC_OK) {
            set_err(ar == HTTPC_ERR_HTTP ? "response too large" : "oom body", 0);
            rc = ar;
            free(raw.memory);
            goto cleanup;
        }
    }

    char *sep = NULL;
    if (raw.size >= 4) {
        for (size_t i = 0; i + 3 < raw.size; i++) {
            if (raw.memory[i] == '\r' && raw.memory[i+1] == '\n' &&
                raw.memory[i+2] == '\r' && raw.memory[i+3] == '\n') {
                sep = raw.memory + i;
                break;
            }
        }
    }
    if (!sep) {
        set_err("malformed response (no header terminator)", 0);
        free(raw.memory);
        rc = HTTPC_ERR_HTTP;
        goto cleanup;
    }

    if (strncmp(raw.memory, "HTTP/1.", 7) != 0 || raw.size < 12) {
        set_err("malformed status line", 0);
        free(raw.memory);
        rc = HTTPC_ERR_HTTP;
        goto cleanup;
    }
    int status = atoi(raw.memory + 9);
    if (status != 200) {
        snprintf(g_last_err, sizeof g_last_err, "HTTP %d", status);
        free(raw.memory);
        rc = HTTPC_ERR_HTTP;
        goto cleanup;
    }

    size_t header_len = (size_t)(sep - raw.memory);
    char *body     = sep + 4;
    size_t body_len = raw.size - (size_t)(body - raw.memory);

    if (contains_ci(raw.memory, header_len, "transfer-encoding: chunked")) {
        rc = decode_chunked(body, body_len, out);
        free(raw.memory);
        if (rc != HTTPC_OK) { set_err("chunked decode failed", 0); goto cleanup; }
    } else {
        size_t want;
        if (find_content_length(raw.memory, header_len, &want)) {
            if (body_len < want) {
                set_err("truncated response (body < Content-Length)", 0);
                free(raw.memory);
                rc = HTTPC_ERR_HTTP;
                goto cleanup;
            }
            if (body_len > want) body_len = want;
        }
        rc = buffer_append(out, body, body_len);
        free(raw.memory);
        if (rc != HTTPC_OK) { set_err("oom out", 0); goto cleanup; }
    }

    if (use_tls) mbedtls_ssl_close_notify(&ssl);
    rc = HTTPC_OK;

cleanup:
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    mbedtls_net_free(&net);
    return rc;
}
