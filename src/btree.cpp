/*
 * ============================================================================
 *                       B+TREE NODE BINARY LAYOUT (4KB PAGE)
 * ============================================================================
 *
 * 1. OVERALL NODE LAYOUT:
 * +--------+--------+------------------+-------------------+------------------------------+---------+
 * |  type  | nkeys  | child pointers   |    kv offsets     |         kv pairs data        | unused  |
 * | (2 B)  | (2 B)  | (nkeys * 8 B)    |  (nkeys * 2 B)    | (variable size concatenated) | (bytes) |
 * +--------+--------+------------------+-------------------+------------------------------+---------+
 *
 * Field Descriptions:
 * - type      (2B): 1 = BNODE_NODE (Internal), 2 = BNODE_LEAF (Leaf).
 * - nkeys     (2B): Total number of keys or child pointers stored in this node.
 * - pointers  (nkeys * 8B): Array of uint64_t child page IDs (unused/zeroed in Leaf nodes).
 * - offsets   (nkeys * 2B): End-offset lookup table relative to start of kv-pairs data section.
 * - kv pairs  (Variable): Packed sequence of key-value records.
 * - unused    (Variable): Zeroed padding ensuring total serialized size fits within 4096 bytes.
 *
 * ----------------------------------------------------------------------------
 *
 * 2. KEY-VALUE PAIR RECORD FORMAT (inside 'kv pairs data'):
 * +---------------+---------------+--------------------+----------------------+
 * | key_size (2B) | val_size (2B) |     key bytes      |      val bytes       |
 * +---------------+---------------+--------------------+----------------------+
 *
 * Field Descriptions:
 * - key_size (2B): Byte length of key payload.
 * - val_size (2B): Byte length of value payload (always 0 for Internal nodes).
 * - key      (...): Raw key bytes.
 * - val      (...): Raw value bytes (omitted if val_size == 0).
 *
 * ============================================================================
 */

#include "btree.h"

#include <cassert>
#include <cstring>
#include <stdexcept>

// Helper functions used to pack/unpack fixed width ints during encode and decode
namespace {
    void write_u16(uint8_t* ptr, uint16_t val) {
        std::memcpy(ptr, &val, sizeof(val));
    }

    void write_u64(uint8_t* ptr, uint64_t val) {
        std::memcpy(ptr, &val, sizeof(val));
    }

    uint16_t read_u16(const uint8_t* ptr) {
        uint16_t val;
        std::memcpy(&val, ptr, sizeof(val));
        return val;
    }

    uint64_t read_u64(const uint8_t* ptr) {
        uint64_t val;
        std::memcpy(&val, ptr, sizeof(val));
        return val;
    }
}

// ============================================================================
// BNode: header
// ============================================================================
uint16_t BNode::btype() const {
    return read_u16(raw() + 0);
}

uint16_t BNode::nkeys() const {
    return read_u16(raw() + 2);
}

void BNode::set_header(uint16_t type, uint16_t nkeys) {
    write_u16(raw() + 0, type);
    write_u16(raw() + 2, nkeys);
}

// ============================================================================
// BNode: layout helpers
// ============================================================================
size_t BNode::ptrs_offset() {
    return 4;
}

size_t BNode::offsets_offset() const {
    return ptrs_offset() + (static_cast<size_t>(nkeys()) * 8);
}

size_t BNode::kv_base_offset() const {
    return offsets_offset() + (static_cast<size_t>(nkeys()) * 2);
}

// ============================================================================
// BNode: child pointers
// ============================================================================
uint64_t BNode::get_ptr(uint16_t idx) const {
    return read_u64(raw() + ptrs_offset() + (static_cast<size_t>(idx) * 8));
}

void BNode::set_ptr(uint16_t idx, uint64_t ptr) {
    write_u64(raw() + ptrs_offset() + (static_cast<size_t>(idx) * 8), ptr);
}

// ============================================================================
// BNode: KV offsets table
// ============================================================================
uint16_t BNode::get_offset(uint16_t idx) const {
    if (idx == 0) {
        return 0;
    }
    return read_u16(raw() + offsets_offset() + (static_cast<size_t>(idx - 1) * 2));
}

void BNode::set_offset(uint16_t idx, uint16_t offset) {
    write_u16(raw() + offsets_offset() + (static_cast<size_t>(idx - 1) * 2), offset);
}

// ============================================================================
// BNode: KV data
// ============================================================================
size_t BNode::kv_pos(uint16_t idx) const {
    return kv_base_offset() + get_offset(idx);
}

std::vector<uint8_t> BNode::get_key(uint16_t idx) const {
    size_t pos = kv_pos(idx);
    uint16_t klen = read_u16(raw() + pos);

    const uint8_t* key_start = raw() + pos + 4; // Skip 2B klen + 2B vlen
    return std::vector<uint8_t>(key_start, key_start + klen);
}

std::vector<uint8_t> BNode::get_val(uint16_t idx) const {
    size_t pos = kv_pos(idx);
    uint16_t klen = read_u16(raw() + pos);
    uint16_t vlen = read_u16(raw() + pos + 2);

    const uint8_t* val_start = raw() + pos + 4 + klen;
    return std::vector<uint8_t>(val_start, val_start + vlen);
}

uint16_t BNode::nbytes() const {
    return static_cast<uint16_t>(kv_pos(nkeys()));
}

void BNode::node_append_kv(uint16_t idx, uint64_t ptr, const std::vector<uint8_t>& key, const std::vector<uint8_t>& val) {
    // ptrs
    set_ptr(idx, ptr);

    // KVs
    size_t pos = kv_pos(idx); // uses the offset value of the previous key
    uint16_t klen = static_cast<uint16_t>(key.size());
    uint16_t vlen = static_cast<uint16_t>(val.size());

    assert(key.size() <= BTREE_MAX_KEY_SIZE);
    assert(val.size() <= BTREE_MAX_VAL_SIZE);
    assert(pos + 4 + klen + vlen <= size());

    // 4-byte KV sizes and KV data
    write_u16(raw() + pos + 0, klen);
    write_u16(raw() + pos + 2, vlen);
    std::memcpy(raw() + pos + 4, key.data(), klen);
    std::memcpy(raw() + pos + 4 + klen, val.data(), vlen);

    // update the offset value for the next key
    set_offset(idx + 1, static_cast<uint16_t>(get_offset(idx) + 4 + klen + vlen));
}

// ============================================================================
// encode / decode
// ============================================================================

// encode copies the used portion of an in-memory node into a fixed
// BTREE_PAGE_SIZE page, ready to be written to disk.
std::vector<uint8_t> encode(const BNode& node) {
    uint16_t used = node.nbytes();
    assert(used <= BTREE_PAGE_SIZE);

    std::vector<uint8_t> page(BTREE_PAGE_SIZE, 0);
    std::memcpy(page.data(), node.raw(), used);
    return page;
}

// decode wraps a page read from disk in a BNode.
BNode decode(const std::vector<uint8_t>& page) {
    assert(page.size() == BTREE_PAGE_SIZE);
    return BNode(page);
}

// ============================================================================
// Leaf insert / update
// ============================================================================
void node_append_range(
    BNode& new_node, const BNode& old, uint16_t dst_new, uint16_t src_old, uint16_t n
) {
    for (uint16_t i = 0; i < n; ++i) {
        uint16_t dst = dst_new + i;
        uint16_t src = src_old + i;
        new_node.node_append_kv(dst, old.get_ptr(src), old.get_key(src), old.get_val(src));
    }
}

void leaf_insert(
    BNode& new_node, const BNode& old, uint16_t idx,
    const std::vector<uint8_t>& key, const std::vector<uint8_t>& val
) {
    new_node.set_header(BNODE_LEAF, old.nkeys() + 1);
    node_append_range(new_node, old, 0, 0, idx);              // keys before idx
    new_node.node_append_kv(idx, 0, key, val);                // the new key
    node_append_range(new_node, old, idx + 1, idx, old.nkeys() - idx); // keys after idx
}

void leaf_update(
    BNode& new_node, const BNode& old, uint16_t idx,
    const std::vector<uint8_t>& key, const std::vector<uint8_t>& val
) {
    new_node.set_header(BNODE_LEAF, old.nkeys());
    node_append_range(new_node, old, 0, 0, idx);
    new_node.node_append_kv(idx, 0, key, val);
    node_append_range(new_node, old, idx + 1, idx + 1, old.nkeys() - (idx + 1));
}

// ============================================================================
// Lookup
// ============================================================================
int64_t node_lookup_le(const BNode& node, const std::vector<uint8_t>& key) {
    int64_t lo = 0, hi = static_cast<int64_t>(node.nkeys()) - 1;
    int64_t result = -1;
    while (lo <= hi) {
        int64_t mid = lo + (hi - lo) / 2;
        std::vector<uint8_t> cur = node.get_key(static_cast<uint16_t>(mid));
        if (cur == key) {
            return mid;
        }
        if (cur < key) {
            result = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return result;
}

// ============================================================================
// Split
// ============================================================================
// Splits an oversized `old` node into `left` and `right`. `left`/`right` must
// each have capacity >= BTREE_PAGE_SIZE. A 3rd node is never needed, since
// BTREE_MAX_KEY_SIZE/BTREE_MAX_VAL_SIZE keep every KV entry within half a
// page (see the static_assert in btree.h).
void node_split(BNode& left, BNode& right, const BNode& old) {
    assert(old.nkeys() >= 2);

    uint16_t nleft = old.nkeys() / 2;

    uint16_t left_bytes = static_cast<uint16_t>(4 + 8 * nleft + 2 * nleft + old.get_offset(nleft));
    while (left_bytes > BTREE_PAGE_SIZE) {
        --nleft;
        left_bytes = static_cast<uint16_t>(4 + 8 * nleft + 2 * nleft + old.get_offset(nleft));
    }
    assert(nleft >= 1);

    uint16_t right_bytes = static_cast<uint16_t>(old.nbytes() - left_bytes + 4);
    while (right_bytes > BTREE_PAGE_SIZE) {
        ++nleft;
        left_bytes = static_cast<uint16_t>(4 + 8 * nleft + 2 * nleft + old.get_offset(nleft));
        right_bytes = static_cast<uint16_t>(old.nbytes() - left_bytes + 4);
    }
    assert(nleft < old.nkeys());
    uint16_t nright = old.nkeys() - nleft;

    left.set_header(old.btype(), nleft);
    right.set_header(old.btype(), nright);
    node_append_range(left, old, 0, 0, nleft);
    node_append_range(right, old, 0, nleft, nright);

    assert(left.nbytes() <= BTREE_PAGE_SIZE);
    assert(right.nbytes() <= BTREE_PAGE_SIZE);
}

std::vector<BNode> node_split_if_needed(const BNode& node) {
    if (node.nbytes() <= BTREE_PAGE_SIZE) {
        return {node};
    }
    BNode left, right;
    node_split(left, right, node);
    return {left, right};
}

// ============================================================================
// Tree insertion
// ============================================================================
BNode tree_insert(
    const BTree& tree, const BNode& node,
    const std::vector<uint8_t>& key, const std::vector<uint8_t>& val
) {
    BNode new_node(2 * BTREE_PAGE_SIZE);
    int64_t idx = node_lookup_le(node, key);

    switch (node.btype()) {
        case BNODE_LEAF:
            if (idx >= 0 && node.get_key(static_cast<uint16_t>(idx)) == key) {
                leaf_update(new_node, node, static_cast<uint16_t>(idx), key, val);
            } else {
                leaf_insert(new_node, node, static_cast<uint16_t>(idx + 1), key, val);
            }
            break;
        case BNODE_NODE:
            assert(idx >= 0);
            node_insert(tree, new_node, node, static_cast<uint16_t>(idx), key, val);
            break;
        default:
            assert(false && "tree_insert: bad node type");
    }
    return new_node;
}

void node_insert(
    const BTree& tree, BNode& new_node, const BNode& old, uint16_t idx,
    const std::vector<uint8_t>& key, const std::vector<uint8_t>& val
) {
    uint64_t child_ptr = old.get_ptr(idx);
    BNode updated_child = tree_insert(tree, tree.pages->get(child_ptr), key, val);

    std::vector<BNode> split = node_split_if_needed(updated_child);
    tree.pages->del(child_ptr);

    node_replace_child_n(tree, new_node, old, idx, split);
}

void node_replace_child_n(
    const BTree& tree, BNode& new_node, const BNode& old, uint16_t idx,
    const std::vector<BNode>& children
) {
    uint16_t inc = static_cast<uint16_t>(children.size());
    new_node.set_header(BNODE_NODE, old.nkeys() + inc - 1);
    node_append_range(new_node, old, 0, 0, idx);
    for (uint16_t i = 0; i < inc; ++i) {
        new_node.node_append_kv(idx + i, tree.pages->new_page(children[i]), children[i].get_key(0), {});
    }
    node_append_range(new_node, old, idx + inc, idx + 1, old.nkeys() - (idx + 1));
}

// ============================================================================
// BTree Insert
// ============================================================================
namespace {
    void check_limit(const std::vector<uint8_t>& key, const std::vector<uint8_t>& val) {
        if (key.empty()) {
            throw std::invalid_argument("empty key");
        }
        if (key.size() > BTREE_MAX_KEY_SIZE) {
            throw std::invalid_argument("key too large");
        }
        if (val.size() > BTREE_MAX_VAL_SIZE) {
            throw std::invalid_argument("value too large");
        }
    }
}

void BTree::insert(const std::vector<uint8_t>& key, const std::vector<uint8_t>& val) {
    check_limit(key, val);

    if (root == 0) {
        // The first root: a leaf holding a dummy sentinel key so a lookup always finds a position.
        BNode new_root(BTREE_PAGE_SIZE);
        new_root.set_header(BNODE_LEAF, 2);
        new_root.node_append_kv(0, 0, {}, {}); // an empty std::vector is lexicographically smaller than an non empty std::vector
        new_root.node_append_kv(1, 0, key, val);
        root = pages->new_page(new_root);
        return;
    }

    BNode updated = tree_insert(*this, pages->get(root), key, val);
    std::vector<BNode> split = node_split_if_needed(updated);
    pages->del(root);

    if (split.size() > 1) {
        // The root was split: add a new level.
        BNode new_root(BTREE_PAGE_SIZE);
        new_root.set_header(BNODE_NODE, static_cast<uint16_t>(split.size()));
        for (uint16_t i = 0; i < split.size(); ++i) {
            uint64_t ptr = pages->new_page(split[i]);
            new_root.node_append_kv(i, ptr, split[i].get_key(0), {});
        }
        root = pages->new_page(new_root);
    } else {
        root = pages->new_page(split[0]);
    }
}
