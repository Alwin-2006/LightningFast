#pragma once

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <vector>

struct Buffer {
    std::vector<uint8_t> storage;
    size_t begin = 0;
    size_t end   = 0;

    size_t size() const { return end - begin; }
    bool  empty() const { return begin == end; }

    uint8_t       *data()       { return storage.data() + begin; }
    const uint8_t *data() const { return storage.data() + begin; }

    uint8_t operator[](size_t i) const { return storage[begin + i]; }

    void append(const uint8_t *src, size_t len) {
        size_t free_at_back = storage.size() - end;
        if (free_at_back >= len) {
            memcpy(storage.data() + end, src, len);
            end += len;
            return;
        }
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
        size_t new_cap = std::max(storage.size() * 2, live + len + 64);
        std::vector<uint8_t> next(new_cap);
        memcpy(next.data(), storage.data() + begin, live);
        storage = std::move(next);
        begin = 0;
        end   = live;
        memcpy(storage.data() + end, src, len);
        end += len;
    }

    void consume(size_t n) {
        assert(n <= size());
        begin += n;
        if (begin == end) { begin = end = 0; }
    }
};
