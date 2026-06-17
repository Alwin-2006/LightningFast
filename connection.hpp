#pragma once

#include "conn.hpp"

Conn *handle_accept(int fd);
void handle_read(Conn *conn);
void handle_write(Conn *conn);
