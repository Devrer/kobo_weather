#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stddef.h>

struct MemoryStruct {
    char  *memory;
    size_t size;
};

enum {
    HTTPC_OK            =  0,
    HTTPC_ERR_URL       = -1,
    HTTPC_ERR_DNS       = -2,
    HTTPC_ERR_CONNECT   = -3,
    HTTPC_ERR_TLS       = -4,
    HTTPC_ERR_IO        = -5,
    HTTPC_ERR_HTTP      = -6,
    HTTPC_ERR_OOM       = -7,
    HTTPC_ERR_CLOCK     = -8,
    HTTPC_ERR_INTERNAL  = -9
};

int         https_get(const char *url, const char *user_agent, struct MemoryStruct *out);
const char *http_client_strerror(void);

#endif
