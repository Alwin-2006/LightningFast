#include "connection.hpp"

#include "common.hpp"
#include "protocol.hpp"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

Conn *handle_accept(int fd) {
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

void handle_write(Conn *conn) {
    assert(conn->outgoing.size() > 0);
    ssize_t rv = write(conn->fd, conn->outgoing.data(), conn->outgoing.size());
    if (rv < 0 && errno == EAGAIN) {
        return;
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
}

void handle_read(Conn *conn) {
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

    while (try_one_request(conn)) {}

    if (!conn->outgoing.empty()) {
        conn->want_read  = false;
        conn->want_write = true;
        return handle_write(conn);
    }
}
