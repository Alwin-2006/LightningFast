#pragma once

#include "buffer.hpp"

struct Conn {
    int fd = -1;
    bool want_read  = false;
    bool want_write = false;
    bool want_close = false;
    Buffer incoming;
    Buffer outgoing;
};
