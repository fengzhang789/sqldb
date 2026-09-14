#include "btree_iter.h"
#include "pagemanager.h"

#include <cassert>

bool cmp_ok(const std::vector<uint8_t>& key, CMP cmp, const std::vector<uint8_t>& ref) {
    switch (cmp) {
        case CMP_GE: return key >= ref;
        case CMP_GT: return key > ref;
        case CMP_LT: return key < ref;
        case CMP_LE: return key <= ref;
    }
    assert(false && "cmp_ok: bad cmp");
    return false;
}

// ============================================================================
// BIter
// ============================================================================
namespace {
    // Reloads the node below `level` from its (just moved) position, placed at its first or last key.
    void reload_kid(BIter& iter, size_t level, bool at_last) {
        if (level + 1 == iter.path.size()) {
            return;
        }
        BNode kid = iter.tree->pages->get(iter.path[level].get_ptr(iter.pos[level]));
        iter.pos[level + 1] = at_last ? static_cast<uint16_t>(kid.nkeys() - 1) : 0;
        iter.path[level + 1] = std::move(kid);
    }

    // Steps `level` back, borrowing from the levels above; returns false, changing nothing, at the tree's first position.
    bool iter_prev(BIter& iter, size_t level) {
        if (iter.pos[level] > 0) {
            --iter.pos[level]; // move within this node
        } else if (level == 0 || !iter_prev(iter, level - 1)) {
            return false; // every level is at its first position
        }
        reload_kid(iter, level, true);
        return true;
    }

    // Mirror of iter_prev: descends into the first key of the next subtree.
    bool iter_next(BIter& iter, size_t level) {
        if (iter.pos[level] + 1 < iter.path[level].nkeys()) {
            ++iter.pos[level];
        } else if (level == 0 || !iter_next(iter, level - 1)) {
            return false;
        }
        reload_kid(iter, level, false);
        return true;
    }
}

std::pair<std::vector<uint8_t>, std::vector<uint8_t>> BIter::deref() const {
    assert(valid());
    const BNode& leaf = path.back();
    return {leaf.get_key(pos.back()), leaf.get_val(pos.back())};
}

bool BIter::valid() const {
    if (path.empty() || pos.back() >= path.back().nkeys()) {
        return false;
    }
    return pos.back() > 0 || !path.back().get_key(0).empty(); // only the sentinel has an empty key
}

void BIter::prev() {
    if (!path.empty()) {
        iter_prev(*this, path.size() - 1); // a no-op at the sentinel
    }
}

void BIter::next() {
    if (!path.empty() && !iter_next(*this, path.size() - 1)) {
        pos.back() = path.back().nkeys(); // after the last key
    }
}

// ============================================================================
// BTree Seek
// ============================================================================
BIter BTree::seek_le(const std::vector<uint8_t>& key) const {
    BIter iter;
    iter.tree = this;
    for (uint64_t ptr = root; ptr != 0;) {
        BNode node = pages->get(ptr);
        int64_t idx = node_lookup_le(node, key);
        assert(idx >= 0); // the sentinel's empty key is <= every key
        ptr = node.btype() == BNODE_NODE ? node.get_ptr(static_cast<uint16_t>(idx)) : 0;
        iter.path.push_back(std::move(node));
        iter.pos.push_back(static_cast<uint16_t>(idx));
    }
    return iter;
}

BIter BTree::seek(const std::vector<uint8_t>& key, CMP cmp) const {
    BIter iter = seek_le(key);
    // seek_le lands on the last key <= `key` (or before the first key), so one step fixes any off-by-one
    bool ok = iter.valid() && cmp_ok(iter.deref().first, cmp, key);
    if (!ok && cmp > 0) {
        iter.next();
    } else if (!ok && iter.valid()) {
        iter.prev(); // CMP_LT on an exact match
    }
    return iter;
}
