#pragma once

#include "conn.hpp"

constexpr size_t k_max_msg = 32 << 20;  // 32 MB

bool try_one_request(Conn *conn);
