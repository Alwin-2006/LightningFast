#pragma once

#include "buffer.hpp"

#include <string>

void resp_bulk(Buffer &out, const std::string &s);
void resp_null(Buffer &out);
void resp_int(Buffer &out, long long val);
void resp_err(Buffer &out, const char *msg);
