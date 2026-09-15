#pragma once

#include <cstdint>

// KVReader is a read-only snapshot of one KV version.
struct KVReader {
    uint64_t version = 0;
    int heap_index = -1; // slot in KV's ReaderHeap; -1 while not in one
};
