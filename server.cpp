// stdlib
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
// system
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
// C++
#include <vector>
#include <string>

static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void msg_errno(const char *msg) {
    fprintf(stderr, "[errno:%d] %s\n", errno, msg);
}

static void die(const char *msg) {
    fprintf(stderr, "[%d] %s\n", errno, msg);
    abort();
}

static void fd_set_nb(int fd) {
    errno = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if (errno) {
        die("fcntl error");
        return;
    }
    flags |= O_NONBLOCK;
    errno = 0;
    (void)fcntl(fd, F_SETFL, flags);
    if (errno) {
        die("fcntl error");
    }
}

// §6b.9 — Set large enough to span multiple event loop iterations
// when testing with a giant pipelined payload.
const size_t k_max_msg = 32 << 20;  // 32 MB, likely larger than the kernel buffer

// ─────────────────────────────────────────────────────────────────────────────
// §6b.9  Better buffer: O(1) front-removal
//
// std::vector used as a FIFO requires shifting all remaining bytes on every
// buf_consume(), giving O(N²) cost when draining many pipelined messages.
//
// This Buffer keeps two pointers inside a heap allocation:
//   - data_begin: advances on consume  → O(1), no data movement
//   - data_end:   advances on append   → amortised O(1), doubles on resize
//
// Layout:
//   ┌──────────────┬────────────────┬──────────────┐
//   │  dead space  │  live data     │  free space  │
//   └──────────────┴────────────────┴──────────────┘
//   ^              ^                ^              ^
//   storage      begin            end          capacity
//
// When free space runs out we either:
//   (a) slide live data to the front if there is enough dead space, or
//   (b) reallocate with doubled capacity.
// ─────────────────────────────────────────────────────────────────────────────
struct Buffer {
    std::vector<uint8_t> storage;
    size_t begin = 0;  // index of first live byte
    size_t end   = 0;  // index one past last live byte

    // Number of bytes currently held
    size_t size() const { return end - begin; }
    bool  empty() const { return begin == end; }

    // Pointer to the first live byte (safe only when size() > 0)
    uint8_t       *data()       { return storage.data() + begin; }
    const uint8_t *data() const { return storage.data() + begin; }

    // Read live bytes by index (no bounds check — caller's responsibility)
    uint8_t operator[](size_t i) const { return storage[begin + i]; }

    // Append len bytes to the back — amortised O(1)
    void append(const uint8_t *src, size_t len) {
        size_t free_at_back = storage.size() - end;
        if (free_at_back >= len) {
            // Fast path: room at the back already
            memcpy(storage.data() + end, src, len);
            end += len;
            return;
        }
        // Try sliding live data to the front to reclaim dead space
        size_t live = size();
        size_t free_total = storage.size() - live;
        if (free_total >= len) {
            memmove(storage.data(), storage.data() + begin, live);
            begin = 0;
            end   = live;
            memcpy(storage.data() + end, src, len);
            end += len;
            return;
        }
        // Must reallocate: grow to at least (live + len), doubling for amortisation
        size_t new_cap = std::max(storage.size() * 2, live + len + 64);
        std::vector<uint8_t> next(new_cap);
        memcpy(next.data(), storage.data() + begin, live);
        storage = std::move(next);
        begin = 0;
        end   = live;
        memcpy(storage.data() + end, src, len);
        end += len;
    }

    // §6b.9: O(1) front-removal — just advance the pointer, no data movement
    void consume(size_t n) {
        assert(n <= size());
        begin += n;
        // Reset to zero when the buffer drains completely so dead space
        // doesn't accumulate indefinitely.
        if (begin == end) { begin = end = 0; }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Connection state
// ─────────────────────────────────────────────────────────────────────────────
struct Conn {
    int fd = -1;
    bool want_read  = false;
    bool want_write = false;
    bool want_close = false;
    Buffer incoming;
    Buffer outgoing;
};

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

// Find \r\n inside the Buffer's live region, starting at live-byte offset `off`.
// Returns the live-byte offset of '\r', or -1 if not found.
static ssize_t find_crlf(const Buffer &buf, size_t off) {
    for (size_t i = off; i + 1 < buf.size(); ++i) {
        if (buf[i] == '\r' && buf[i + 1] == '\n')
            return (ssize_t)i;
    }
    return -1;
}

// Parse a decimal integer from buf[off..] up to the next \r\n.
// Advances `off` past the \r\n on success.
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

// ─────────────────────────────────────────────────────────────────────────────
// Accept
// ─────────────────────────────────────────────────────────────────────────────
static Conn *handle_accept(int fd) {
    struct sockaddr_in client_addr = {};
    socklen_t addrlen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);
    if (connfd < 0) {
        msg_errno("accept() error");
        return NULL;
    }
    uint32_t ip = client_addr.sin_addr.s_addr;
    fprintf(stderr, "new client from %u.%u.%u.%u:%u\n",
        ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24,
        ntohs(client_addr.sin_port));

    fd_set_nb(connfd);

    Conn *conn = new Conn();
    conn->fd        = connfd;
    conn->want_read = true;
    return conn;
}

// ─────────────────────────────────────────────────────────────────────────────
// §6b.6  Process exactly one RESP request from conn->incoming.
//
// Returns true  → a complete request was parsed and a response appended;
//                 caller should loop to handle pipelined messages.
// Returns false → not enough data yet (or error, which sets want_close).
//
// Uses buf_consume() per message, NOT a buffer clear, so that any following
// pipelined messages remain in the buffer for the next iteration.
// ─────────────────────────────────────────────────────────────────────────────
static bool try_one_request(Conn *conn) {
    if (conn->incoming.empty()) return false;

    size_t off = 0;

    // RESP Array header: '*'
    if (conn->incoming[off] != '*') {
        msg("Protocol error: expected RESP Array (*)");
        conn->want_close = true;
        return false;
    }
    off++;

    long arr_len = 0;
    if (!parse_int(conn->incoming, off, arr_len)) return false;  // incomplete

    if (arr_len <= 0 || arr_len > 10000) {
        msg("Protocol error: invalid array length");
        conn->want_close = true;
        return false;
    }

    std::vector<std::string> args;
    args.reserve((size_t)arr_len);

    for (long i = 0; i < arr_len; ++i) {
        if (off >= conn->incoming.size()) return false;  // incomplete

        if (conn->incoming[off] != '$') {
            msg("Protocol error: expected Bulk String ($)");
            conn->want_close = true;
            return false;
        }
        off++;

        long str_len = 0;
        if (!parse_int(conn->incoming, off, str_len)) return false;  // incomplete

        if (str_len < 0 || (size_t)str_len > k_max_msg) {
            msg("Protocol error: invalid bulk string length");
            conn->want_close = true;
            return false;
        }

        // Need str_len bytes of payload + trailing \r\n
        if (off + (size_t)str_len + 2 > conn->incoming.size()) return false;  // incomplete

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

    // Log
    printf("client says RESP Array (%zu elements):\n", args.size());
    for (size_t i = 0; i < args.size(); ++i)
        printf("  [%zu]: %s\n", i, args[i].c_str());

    // Echo the first argument as a RESP Bulk String response
    const std::string echo_val = args.empty() ? std::string("") : args[0];
    char hdr[64];
    int hdr_len = snprintf(hdr, sizeof(hdr), "$%zu\r\n", echo_val.size());
    conn->outgoing.append((const uint8_t *)hdr, (size_t)hdr_len);
    conn->outgoing.append((const uint8_t *)echo_val.data(), echo_val.size());
    conn->outgoing.append((const uint8_t *)"\r\n", 2);

    // §6b.6: consume only this message's bytes, leaving any pipelined
    // messages untouched so the while-loop in handle_read() can process them.
    conn->incoming.consume(off);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// §6b.8  handle_write — must guard against EAGAIN
//
// The optimistic write called directly from handle_read() may run when the
// kernel send-buffer is full (e.g. during heavy pipelining).  Returning early
// on EAGAIN is mandatory; the event loop will call us again via POLLOUT.
// ─────────────────────────────────────────────────────────────────────────────
static void handle_write(Conn *conn) {
    assert(conn->outgoing.size() > 0);
    ssize_t rv = write(conn->fd, conn->outgoing.data(), conn->outgoing.size());
    if (rv < 0 && errno == EAGAIN) {
        return;  // §6b.8: kernel send-buffer full — event loop will retry
    }
    if (rv < 0) {
        msg_errno("write() error");
        conn->want_close = true;
        return;
    }

    conn->outgoing.consume((size_t)rv);

    if (conn->outgoing.empty()) {
        conn->want_read  = true;
        conn->want_write = false;
    }
    // else: still have data → stay in want_write state
}

// ─────────────────────────────────────────────────────────────────────────────
// handle_read
// ─────────────────────────────────────────────────────────────────────────────
static void handle_read(Conn *conn) {
    uint8_t buf[64 * 1024];
    ssize_t rv = read(conn->fd, buf, sizeof(buf));
    if (rv < 0 && errno == EAGAIN) return;
    if (rv < 0) {
        msg_errno("read() error");
        conn->want_close = true;
        return;
    }
    if (rv == 0) {
        msg(conn->incoming.empty() ? "client closed" : "unexpected EOF");
        conn->want_close = true;
        return;
    }
    conn->incoming.append(buf, (size_t)rv);

    // §6b.6: loop — one read() may deliver multiple pipelined RESP messages
    while (try_one_request(conn)) {}

    if (!conn->outgoing.empty()) {
        conn->want_read  = false;
        conn->want_write = true;
        // §6b.8: Optimistic write — in request-response protocols the client
        // has consumed previous data, so the socket is likely writable right
        // now.  Attempt the write immediately to save one poll()/syscall
        // round-trip.  handle_write() handles EAGAIN safely if we're wrong.
        return handle_write(conn);
    }
    // else: no response yet → stay in want_read
}

// ─────────────────────────────────────────────────────────────────────────────
// main — event loop (unchanged structure)
// ─────────────────────────────────────────────────────────────────────────────
int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket()");

    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = ntohs(6379);
    addr.sin_addr.s_addr = ntohl(0);
    int rv = bind(fd, (const sockaddr *)&addr, sizeof(addr));
    if (rv) die("bind()");

    fd_set_nb(fd);

    rv = listen(fd, SOMAXCONN);
    if (rv) die("listen()");

    std::vector<Conn *> fd2conn;
    std::vector<struct pollfd> poll_args;

    while (true) {
        poll_args.clear();
        struct pollfd pfd = {fd, POLLIN, 0};
        poll_args.push_back(pfd);

        for (Conn *conn : fd2conn) {
            if (!conn) continue;
            struct pollfd pfd = {conn->fd, POLLERR, 0};
            if (conn->want_read)  pfd.events |= POLLIN;
            if (conn->want_write) pfd.events |= POLLOUT;
            poll_args.push_back(pfd);
        }

        rv = poll(poll_args.data(), (nfds_t)poll_args.size(), -1);
        if (rv < 0 && errno == EINTR) continue;
        if (rv < 0) die("poll");

        if (poll_args[0].revents) {
            if (Conn *conn = handle_accept(fd)) {
                if (fd2conn.size() <= (size_t)conn->fd)
                    fd2conn.resize(conn->fd + 1);
                assert(!fd2conn[conn->fd]);
                fd2conn[conn->fd] = conn;
            }
        }

        for (size_t i = 1; i < poll_args.size(); ++i) {
            uint32_t ready = poll_args[i].revents;
            if (ready == 0) continue;

            Conn *conn = fd2conn[poll_args[i].fd];
            if (ready & POLLIN) {
                assert(conn->want_read);
                handle_read(conn);
            }
            if (ready & POLLOUT) {
                assert(conn->want_write);
                handle_write(conn);
            }
            if ((ready & POLLERR) || conn->want_close) {
                (void)close(conn->fd);
                fd2conn[conn->fd] = NULL;
                delete conn;
            }
        }
    }
    return 0;
}