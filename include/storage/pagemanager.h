#pragma once

#include <cstdint>

#include "storage/btree.h"

// IPageManager isolates the B+tree data structure from how pages are
// actually stored, so the tree can be tested with an in-memory
// implementation, and read and written through a transaction's view of the
// file (see KVReader and KVTX), without changing any tree logic.
struct IPageManager {
    virtual ~IPageManager() = default;
    virtual BNode get(uint64_t ptr) const = 0;
    virtual uint64_t new_page(const BNode& node) = 0;
    virtual void del(uint64_t ptr) = 0;
};
