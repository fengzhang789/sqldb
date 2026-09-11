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

// Helper functions used to pack/unpack fixed width ints during encode and decode
static void write_u16(uint8_t* ptr, uint16_t val) {
    std::memcpy(ptr, &val, sizeof(val));
}

static void write_u64(uint8_t* ptr, uint64_t val) {
    std::memcpy(ptr, &val, sizeof(val));
}

// encode returns the on disk bit encoding of a btree node as a vector of uint8_t
std::vector<uint8_t> encode(const Node& node) {
    std::vector<uint8_t> page(BTREE_PAGE_SIZE, 0); // page with 4096 zero bytes
    uint16_t nkeys_val = static_cast<uint16_t>(node.pairs.size());

    // Header
    write_u16(page.data() + 0, node.type);
    write_u16(page.data() + 2, nkeys_val);

    const size_t ptrs_offset     = 4; // start of child ptrs
    const size_t offsets_offset  = ptrs_offset + (static_cast<size_t>(nkeys_val) * 8); // start of kv offsets table
    const size_t kv_start_offset = offsets_offset + (static_cast<size_t>(nkeys_val) * 2); // start of raw key value payloads

    // Child Pointers
    for (size_t i = 0; i < nkeys_val; i++) {
        uint64_t ptr_val = (node.type == BNODE_NODE) ? node.children[i] : 0;
        write_u64(page.data() + ptrs_offset + (i * 8), ptr_val);
    }

    // Write KV data and record offsets
    size_t curr_kv_offset = 0;
    for (size_t i = 0; i < nkeys_val; ++i) {
        const KVPair& kv = node.pairs[i];
        uint16_t klen = static_cast<uint16_t>(kv.key.size());
        uint16_t vlen = (node.type == BNODE_LEAF) ? static_cast<uint16_t>(kv.val.size()) : 0;

        // Guard against writing past the end of the page. A node this large
        // should have been split before encode() is ever called.
        assert(kv_start_offset + curr_kv_offset + 4 + klen + vlen <= BTREE_PAGE_SIZE);

        uint8_t* kv_ptr = page.data() + kv_start_offset + curr_kv_offset;

        // Write key and value length, then write key data
        write_u16(kv_ptr + 0, klen);
        write_u16(kv_ptr + 2, vlen);
        std::memcpy(kv_ptr + 4, kv.key.data(), klen);

        // write value only if leaf
        if (vlen > 0) {
            std::memcpy(kv_ptr + 4 + klen, kv.val.data(), vlen);
        }

        curr_kv_offset += (4 + klen + vlen);
        write_u16(page.data() + offsets_offset + (i * 2), static_cast<uint16_t>(curr_kv_offset)); // record in offsets table
    }

    return page;
}

// ============================================================================
// Layout Helpers
// ============================================================================

inline uint16_t btype(const uint8_t* page) {
    uint16_t val;
    std::memcpy(&val, page + 0, sizeof(val));
    return val;
}

inline uint16_t nkeys(const uint8_t* page) {
    uint16_t val;
    std::memcpy(&val, page + 2, sizeof(val));
    return val;
}

inline size_t ptrsOffset() {
    return 4;
}

inline size_t offsetsOffset(const uint8_t* page) {
    return ptrsOffset() + (nkeys(page) * 8);
}

inline size_t kvBaseOffset(const uint8_t* page) {
    return offsetsOffset(page) + (nkeys(page) * 2);
}

// Read child pointer at index n
inline uint64_t getPtr(const uint8_t* page, uint16_t idx) {
    uint64_t ptr;
    std::memcpy(&ptr, page + ptrsOffset() + (idx * 8), sizeof(ptr));
    return ptr;
}

// Read KV end-offset at index n
inline uint16_t getOffset(const uint8_t* page, uint16_t idx) {
    uint16_t offset;
    std::memcpy(&offset, page + offsetsOffset(page) + (idx * 2), sizeof(offset));
    return offset;
}

// Locate starting byte position of the nth KV pair
inline size_t kvPos(const uint8_t* page, uint16_t idx) {
    if (idx == 0) {
        return kvBaseOffset(page);
    }
    return kvBaseOffset(page) + getOffset(page, idx - 1);
}

// Extract key at index n
inline std::vector<uint8_t> getKey(const uint8_t* page, uint16_t idx) {
    size_t pos = kvPos(page, idx);
    uint16_t klen;
    std::memcpy(&klen, page + pos, sizeof(klen));

    const uint8_t* key_start = page + pos + 4; // Skip 2B klen + 2B vlen
    return std::vector<uint8_t>(key_start, key_start + klen);
}

// Extract value at index n
inline std::vector<uint8_t> getVal(const uint8_t* page, uint16_t idx) {
    if (btype(page) != BNODE_LEAF) {
        return {};
    }

    size_t pos = kvPos(page, idx);
    uint16_t klen, vlen;
    std::memcpy(&klen, page + pos, sizeof(klen));
    std::memcpy(&vlen, page + pos + 2, sizeof(vlen));

    const uint8_t* val_start = page + pos + 4 + klen;
    return std::vector<uint8_t>(val_start, val_start + vlen);
}

// decode a Node as a sequence of 4096 bytes into a Node
Node decode(const std::vector<uint8_t>& page) {
    assert(page.size() == BTREE_PAGE_SIZE);
    const uint8_t* buf = page.data();
    Node node;

    // Read Header
    node.type = btype(buf);
    uint16_t count = nkeys(buf);

    // Read Child Pointers (Internal Nodes Only)
    if (node.type == BNODE_NODE) {
        node.children.resize(count);
        for (uint16_t i = 0; i < count; ++i) {
            node.children[i] = getPtr(buf, i);
        }
    }

    // Read Key-Value Pairs
    node.pairs.resize(count);
    for (uint16_t i = 0; i < count; ++i) {
        node.pairs[i].key = getKey(buf, i);
        node.pairs[i].val = getVal(buf, i);
    }

    return node;
}

