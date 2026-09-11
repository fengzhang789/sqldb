#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>

// Constants
constexpr uint16_t BNODE_NODE = 1; // Internal node with child page pointers
constexpr uint16_t BNODE_LEAF = 2; // Leaf node with values
constexpr size_t BTREE_PAGE_SIZE = 4096;
constexpr size_t BTREE_MAX_KEY_SIZE = 1000;
constexpr size_t BTREE_MAX_VAL_SIZE = 1000;

// Per-KV overhead: 8B child pointer + 2B offset entry + 2B key length + 2B val length.
constexpr size_t BNODE_KV_ENTRY_OVERHEAD = 8 + 2 + 2 + 2;

// Every KV entry must fit within half a page, so node_split never needs a 3rd node.
static_assert(
    BNODE_KV_ENTRY_OVERHEAD + BTREE_MAX_KEY_SIZE + BTREE_MAX_VAL_SIZE <= (BTREE_PAGE_SIZE - 4) / 2,
    "BTREE_MAX_KEY_SIZE + BTREE_MAX_VAL_SIZE must leave every KV entry at most "
    "half a page, so node_split never needs a 3rd node"
);

// BNode wraps a single page's worth of bytes (the on-disk B+tree node
// format) and provides accessors/mutators for its layout: header, child
// pointers, KV offset table, and KV data. 
// NOTE: Nodes are copy-on-write: updates are made by building a new BNode 
//       rather than mutating one in place.
struct BNode {
    std::vector<uint8_t> data;

    // A default-constructed BNode is a zeroed page
    explicit BNode(size_t size = BTREE_PAGE_SIZE) : data(size, 0) {}
    explicit BNode(std::vector<uint8_t> buf) : data(std::move(buf)) {}

    uint8_t* raw() { return data.data(); }
    const uint8_t* raw() const { return data.data(); }
    size_t size() const { return data.size(); }

    // Header
    uint16_t btype() const;
    uint16_t nkeys() const;
    void set_header(uint16_t type, uint16_t nkeys);

    // Child pointers (unused/zeroed for leaf nodes)
    uint64_t get_ptr(uint16_t idx) const;
    void set_ptr(uint16_t idx, uint64_t ptr);

    // offset(idx) is the end offset of the KV pair at idx,
    // relative to the start of the KV data section. offset(0) is 0
    uint16_t get_offset(uint16_t idx) const;
    void set_offset(uint16_t idx, uint16_t offset);

    // Layout helpers
    static size_t ptrs_offset();
    size_t offsets_offset() const;
    size_t kv_base_offset() const;

    // Starting byte position of the idx'th KV pair.
    size_t kv_pos(uint16_t idx) const;

    std::vector<uint8_t> get_key(uint16_t idx) const;
    std::vector<uint8_t> get_val(uint16_t idx) const;

    // Number of bytes used by the node (header + ptrs + offsets + KV data).
    uint16_t nbytes() const;

    // Write the idx'th KV pair and its child pointer into the node.
    // Assumes KV pairs are appended in order, since it relies on the
    // offset recorded for the previous KV pair.
    void node_append_kv(uint16_t idx, uint64_t ptr, const std::vector<uint8_t>& key, const std::vector<uint8_t>& val);
};

// encode finalizes an in-memory BNode into a fixed BTREE_PAGE_SIZE page.
std::vector<uint8_t> encode(const BNode& node);
// decode wraps a page read from disk in a BNode.
BNode decode(const std::vector<uint8_t>& page);

// Copy n KV pairs (and their child pointers) starting at src_old in `old`
// into `new_node` starting at dst_new.
void node_append_range(
    BNode& new_node, const BNode& old, uint16_t dst_new, uint16_t src_old, uint16_t n
);

// Build `new_node` as a copy of `old` with (key, val) inserted at idx.
void leaf_insert(
    BNode& new_node, const BNode& old, uint16_t idx,
    const std::vector<uint8_t>& key, const std::vector<uint8_t>& val
);

// Build `new_node` as a copy of `old` with the value at idx replaced by val.
void leaf_update(
    BNode& new_node, const BNode& old, uint16_t idx,
    const std::vector<uint8_t>& key, const std::vector<uint8_t>& val
);

// Find the last position whose key is less than or equal to `key`.
// Returns -1 if all keys are greater than `key`.
int64_t node_lookup_le(const BNode& node, const std::vector<uint8_t>& key);

// Split an oversized node into 2 nodes, each of which fits within
// BTREE_PAGE_SIZE. Requires old.nkeys() >= 2.
void node_split(BNode& left, BNode& right, const BNode& old);
