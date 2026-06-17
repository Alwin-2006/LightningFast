#include "store.hpp"

#include "resp.hpp"

#include <map>
#include <stdio.h>
#include <string>

static std::map<std::string, std::string> g_data;

void do_request(const std::vector<std::string> &args, Buffer &out) {
    if (args.empty()) {
        resp_err(out, "empty command");
        return;
    }

    const std::string &cmd = args[0];

    if (cmd == "get" || cmd == "GET") {
        if (args.size() != 2) {
            resp_err(out, "wrong number of arguments for 'get'");
            return;
        }
        auto it = g_data.find(args[1]);
        if (it == g_data.end()) {
            resp_null(out);
        } else {
            resp_bulk(out, it->second);
        }

    } else if (cmd == "set" || cmd == "SET") {
        if (args.size() != 3) {
            resp_err(out, "wrong number of arguments for 'set'");
            return;
        }
        g_data[args[1]] = args[2];
        resp_bulk(out, "OK");

    } else if (cmd == "del" || cmd == "DEL") {
        if (args.size() < 2) {
            resp_err(out, "wrong number of arguments for 'del'");
            return;
        }
        long long deleted = 0;
        for (size_t i = 1; i < args.size(); ++i) {
            deleted += (long long)g_data.erase(args[i]);
        }
        resp_int(out, deleted);

    } else {
        char errbuf[128];
        snprintf(errbuf, sizeof(errbuf),
                 "unknown command '%s'", cmd.c_str());
        resp_err(out, errbuf);
    }
}
