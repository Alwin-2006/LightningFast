#include "resp.hpp"

#include <stdio.h>
#include <string.h>

void resp_bulk(Buffer &out, const std::string &s) {
    char hdr[64];
    int n = snprintf(hdr, sizeof(hdr), "$%zu\r\n", s.size());
    out.append((const uint8_t *)hdr, (size_t)n);
    out.append((const uint8_t *)s.data(), s.size());
    out.append((const uint8_t *)"\r\n", 2);
}

void resp_null(Buffer &out) {
    out.append((const uint8_t *)"$-1\r\n", 5);
}

void resp_int(Buffer &out, long long val) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), ":%lld\r\n", val);
    out.append((const uint8_t *)buf, (size_t)n);
}

void resp_err(Buffer &out, const char *msg) {
    out.append((const uint8_t *)"-ERR ", 5);
    out.append((const uint8_t *)msg, strlen(msg));
    out.append((const uint8_t *)"\r\n", 2);
}
