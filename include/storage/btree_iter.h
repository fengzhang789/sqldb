#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "storage/btree.h"

// Relations for BTree::seek and range scans; the sign is the scan direction (> 0 walks forward).
enum CMP : int {
    CMP_GE = +3, // >=
    CMP_GT = +2, // >
    CMP_LT = -2, // <
    CMP_LE = -3, // <=
};

// Does `key` satisfy `cmp` relative to `ref`?
bool cmp_ok(const std::vector<uint8_t>& key, CMP cmp, const std::vector<uint8_t>& ref);

// BIter is an iterator over a BTree's KV pairs in key order. BTree::insert's sentinel empty key doubles as the
// before-first position and a leaf position of nkeys() as the after-last one: valid() is false at both, and a single
// step the other way returns to the first/last key.
// NOTE: an iterator must not be used across an update.
struct BIter {
    const BTree* tree = nullptr;
    std::vector<BNode> path; // root to leaf; empty for an empty tree
    std::vector<uint16_t> pos; // index into each node of path

    std::pair<std::vector<uint8_t>, std::vector<uint8_t>> deref() const; // the current KV pair; requires valid()
    bool valid() const;
    void prev();
    void next();
};
