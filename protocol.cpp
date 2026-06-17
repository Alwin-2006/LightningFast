#include "protocol.hpp"

#include "common.hpp"
#include "store.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

static ssize_t find_crlf(const Buffer &buf, size_t off) {
    for (size_t i = off; i + 1 < buf.size(); ++i) {
        if (buf[i] == '\r' && buf[i + 1] == '\n')
            return (ssize_t)i;
    }
    return -1;
}

static bool parse_int(const Buffer &buf, size_t &off, long &out) {
    ssize_t crlf = find_crlf(buf, off);
    if (crlf < 0 || crlf == (ssize_t)off) return false;

    size_t len = (size_t)crlf - off;
    if (len >= 32) return false;

    char tmp[32];
    for (size_t i = 0; i < len; ++i) tmp[i] = (char)buf[off + i];
    tmp[len] = '\0';

    char *end;
    out = strtol(tmp, &end, 10);
    if (*end != '\0') return false;

    off = (size_t)crlf + 2;
    return true;
}

bool try_one_request(Conn *conn) {
    if (conn->incoming.empty()) return false;

    size_t off = 0;

    if (conn->incoming[off] != '*') {
        msg("Protocol error: expected RESP Array (*)");
        conn->want_close = true;
        return false;
    }
    off++;

    long arr_len = 0;
    if (!parse_int(conn->incoming, off, arr_len)) return false;

    if (arr_len <= 0 || arr_len > 10000) {
        msg("Protocol error: invalid array length");
        conn->want_close = true;
        return false;
    }

    std::vector<std::string> args;
    args.reserve((size_t)arr_len);

    for (long i = 0; i < arr_len; ++i) {
        if (off >= conn->incoming.size()) return false;

        if (conn->incoming[off] != '$') {
            msg("Protocol error: expected Bulk String ($)");
            conn->want_close = true;
            return false;
        }
        off++;

        long str_len = 0;
        if (!parse_int(conn->incoming, off, str_len)) return false;

        if (str_len < 0 || (size_t)str_len > k_max_msg) {
            msg("Protocol error: invalid bulk string length");
            conn->want_close = true;
            return false;
        }

        if (off + (size_t)str_len + 2 > conn->incoming.size()) return false;

        if (conn->incoming[off + str_len] != '\r' ||
            conn->incoming[off + str_len + 1] != '\n') {
            msg("Protocol error: bulk string missing trailing CRLF");
            conn->want_close = true;
            return false;
        }

        args.emplace_back(
            reinterpret_cast<const char *>(conn->incoming.data() + off),
            (size_t)str_len
        );
        off += (size_t)str_len + 2;
    }

    printf("cmd: %s (%zu args)\n", args.empty() ? "" : args[0].c_str(), args.size());
    do_request(args, conn->outgoing);

    conn->incoming.consume(off);
    return true;
}
