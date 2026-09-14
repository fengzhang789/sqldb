#include "storage/btree.h"
#include "storage/pagemanager.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <gtest/gtest.h>

namespace {
    std::vector<uint8_t> bytes(const std::string& s) {
        return std::vector<uint8_t>(s.begin(), s.end());
    }

    std::string str(const std::vector<uint8_t>& b) {
        return std::string(b.begin(), b.end());
    }
}

// ============================================================================
// Header
// ============================================================================
TEST(BNodeHeader, WhenHeaderIsSetThenBtypeAndNkeysAreReturned) {
    BNode node;
    node.set_header(BNODE_LEAF, 3);
    EXPECT_EQ(node.btype(), BNODE_LEAF);
    EXPECT_EQ(node.nkeys(), 3);
}

TEST(BNodeHeader, WhenTypeIsSetToNodeThenBtypeIsNode) {
    BNode node;
    node.set_header(BNODE_NODE, 0);
    EXPECT_EQ(node.btype(), BNODE_NODE);
    EXPECT_EQ(node.nkeys(), 0);
}

TEST(BNodeHeader, WhenDefaultConstructedThenAllFieldsAreZero) {
    BNode node;
    EXPECT_EQ(node.size(), BTREE_PAGE_SIZE);
    EXPECT_EQ(node.btype(), 0);
    EXPECT_EQ(node.nkeys(), 0);
}

// ============================================================================
// Layout helpers
// ============================================================================
TEST(BNodeLayout, WhenNkeysIsFiveThenLayoutOffsetsScaleAccordingly) {
    BNode node;
    node.set_header(BNODE_LEAF, 5);

    EXPECT_EQ(BNode::ptrs_offset(), 4u);
    EXPECT_EQ(node.offsets_offset(), 4u + 5u * 8u);
    EXPECT_EQ(node.kv_base_offset(), 4u + 5u * 8u + 5u * 2u);
}

TEST(BNodeLayout, WhenNkeysIsZeroThenOffsetsCollapseToPtrsOffset) {
    BNode node;
    node.set_header(BNODE_LEAF, 0);

    EXPECT_EQ(node.offsets_offset(), BNode::ptrs_offset());
    EXPECT_EQ(node.kv_base_offset(), node.offsets_offset());
    EXPECT_EQ(node.nbytes(), node.kv_base_offset());
}

// ============================================================================
// Child pointers
// ============================================================================
TEST(BNodePointers, WhenPtrIsSetThenGetPtrReturnsSameValue) {
    BNode node;
    node.set_header(BNODE_NODE, 2);
    node.set_ptr(0, 100);
    node.set_ptr(1, 200);

    EXPECT_EQ(node.get_ptr(0), 100u);
    EXPECT_EQ(node.get_ptr(1), 200u);
}

TEST(BNodePointers, WhenPtrIsMaxUint64ThenGetPtrReturnsSameValue) {
    BNode node;
    node.set_header(BNODE_NODE, 1);
    uint64_t big = 0xFFFFFFFFFFFFFFFFull;
    node.set_ptr(0, big);
    EXPECT_EQ(node.get_ptr(0), big);
}

// ============================================================================
// KV offset table
// ============================================================================
TEST(BNodeOffsets, WhenIdxIsZeroThenGetOffsetReturnsZero) {
    BNode node;
    node.set_header(BNODE_LEAF, 1);
    EXPECT_EQ(node.get_offset(0), 0u);
}

TEST(BNodeOffsets, WhenOffsetsAreSetThenGetOffsetReturnsSameValues) {
    BNode node;
    node.set_header(BNODE_LEAF, 3);
    node.set_offset(1, 10);
    node.set_offset(2, 25);

    EXPECT_EQ(node.get_offset(0), 0u);
    EXPECT_EQ(node.get_offset(1), 10u);
    EXPECT_EQ(node.get_offset(2), 25u);
}

// ============================================================================
// node_append_kv / get_key / get_val
// ============================================================================
TEST(BNodeAppendKv, WhenSingleKvIsAppendedThenGetKeyAndGetValReturnIt) {
    BNode node;
    node.set_header(BNODE_LEAF, 1);
    node.node_append_kv(0, 0, bytes("k1"), bytes("hi"));

    EXPECT_EQ(str(node.get_key(0)), "k1");
    EXPECT_EQ(str(node.get_val(0)), "hi");
}

TEST(BNodeAppendKv, WhenMultipleKvsAreAppendedThenEachIsRetrievableByIndex) {
    BNode node;
    node.set_header(BNODE_LEAF, 2);
    node.node_append_kv(0, 0, bytes("k1"), bytes("hi"));
    node.node_append_kv(1, 0, bytes("k3"), bytes("hello"));

    EXPECT_EQ(str(node.get_key(0)), "k1");
    EXPECT_EQ(str(node.get_val(0)), "hi");
    EXPECT_EQ(str(node.get_key(1)), "k3");
    EXPECT_EQ(str(node.get_val(1)), "hello");
}

TEST(BNodeAppendKv, WhenKeyAndValAreEmptyThenGetKeyAndGetValReturnEmpty) {
    BNode node;
    node.set_header(BNODE_LEAF, 1);
    node.node_append_kv(0, 0, {}, {});

    EXPECT_TRUE(node.get_key(0).empty());
    EXPECT_TRUE(node.get_val(0).empty());
}

TEST(BNodeAppendKv, WhenAppendingToInternalNodeThenPtrsAndKeysAreStoredWithEmptyVals) {
    BNode node;
    node.set_header(BNODE_NODE, 2);
    node.node_append_kv(0, 111, bytes("a"), {});
    node.node_append_kv(1, 222, bytes("m"), {});

    EXPECT_EQ(node.get_ptr(0), 111u);
    EXPECT_EQ(node.get_ptr(1), 222u);
    EXPECT_EQ(str(node.get_key(0)), "a");
    EXPECT_EQ(str(node.get_key(1)), "m");
    EXPECT_TRUE(node.get_val(0).empty());
    EXPECT_TRUE(node.get_val(1).empty());
}

TEST(BNodeAppendKv, WhenKvsAreAppendedThenNbytesReflectsHeaderPtrsOffsetsAndKvData) {
    BNode node;
    node.set_header(BNODE_LEAF, 2);
    node.node_append_kv(0, 0, bytes("k1"), bytes("hi"));
    node.node_append_kv(1, 0, bytes("k3"), bytes("hello"));

    // header(4) + ptrs(2*8) + offsets(2*2) + kv1(4+2+2) + kv2(4+2+5)
    size_t expected = 4 + 16 + 4 + (4 + 2 + 2) + (4 + 2 + 5);
    EXPECT_EQ(node.nbytes(), expected);
}

// ============================================================================
// encode / decode
// ============================================================================
TEST(EncodeDecode, WhenNodeIsEncodedThenPageSizeIsFixed) {
    BNode node;
    node.set_header(BNODE_LEAF, 1);
    node.node_append_kv(0, 0, bytes("k"), bytes("v"));

    std::vector<uint8_t> page = encode(node);
    EXPECT_EQ(page.size(), BTREE_PAGE_SIZE);
}

TEST(EncodeDecode, WhenNodeIsEncodedThenUnusedBytesAreZero) {
    BNode node;
    node.set_header(BNODE_LEAF, 1);
    node.node_append_kv(0, 0, bytes("k"), bytes("v"));

    std::vector<uint8_t> page = encode(node);
    for (size_t i = node.nbytes(); i < page.size(); ++i) {
        ASSERT_EQ(page[i], 0) << "nonzero byte at offset " << i;
    }
}

TEST(EncodeDecode, WhenLeafNodeIsEncodedThenDecodedNodeMatchesOriginal) {
    BNode original;
    original.set_header(BNODE_LEAF, 2);
    original.node_append_kv(0, 0, bytes("k1"), bytes("hi"));
    original.node_append_kv(1, 0, bytes("k3"), bytes("hello"));

    BNode decoded = decode(encode(original));

    EXPECT_EQ(decoded.btype(), BNODE_LEAF);
    EXPECT_EQ(decoded.nkeys(), 2);
    EXPECT_EQ(str(decoded.get_key(0)), "k1");
    EXPECT_EQ(str(decoded.get_val(0)), "hi");
    EXPECT_EQ(str(decoded.get_key(1)), "k3");
    EXPECT_EQ(str(decoded.get_val(1)), "hello");
}

TEST(EncodeDecode, WhenInternalNodeIsEncodedThenDecodedNodeMatchesOriginal) {
    BNode original;
    original.set_header(BNODE_NODE, 2);
    original.node_append_kv(0, 10, bytes("a"), {});
    original.node_append_kv(1, 20, bytes("m"), {});

    BNode decoded = decode(encode(original));

    EXPECT_EQ(decoded.btype(), BNODE_NODE);
    EXPECT_EQ(decoded.nkeys(), 2);
    EXPECT_EQ(decoded.get_ptr(0), 10u);
    EXPECT_EQ(decoded.get_ptr(1), 20u);
    EXPECT_EQ(str(decoded.get_key(0)), "a");
    EXPECT_EQ(str(decoded.get_key(1)), "m");
}

TEST(EncodeDecode, WhenPageSizeIsWrongThenDecodeAsserts) {
    std::vector<uint8_t> bad_page(BTREE_PAGE_SIZE - 1, 0);
    EXPECT_DEATH(decode(bad_page), "");
}

// ============================================================================
// node_append_range
// ============================================================================
TEST(NodeAppendRange, WhenRangeIsCopiedThenKeysAndValsMatchSource) {
    BNode old;
    old.set_header(BNODE_LEAF, 3);
    old.node_append_kv(0, 0, bytes("k1"), bytes("v1"));
    old.node_append_kv(1, 0, bytes("k2"), bytes("v2"));
    old.node_append_kv(2, 0, bytes("k3"), bytes("v3"));

    BNode new_node;
    new_node.set_header(BNODE_LEAF, 3);
    node_append_range(new_node, old, 0, 0, 3);

    EXPECT_EQ(str(new_node.get_key(0)), "k1");
    EXPECT_EQ(str(new_node.get_key(1)), "k2");
    EXPECT_EQ(str(new_node.get_key(2)), "k3");
    EXPECT_EQ(str(new_node.get_val(0)), "v1");
    EXPECT_EQ(str(new_node.get_val(1)), "v2");
    EXPECT_EQ(str(new_node.get_val(2)), "v3");
}

TEST(NodeAppendRange, WhenCopyingSubsetThenOnlySelectedKeysAreCopied) {
    BNode old;
    old.set_header(BNODE_LEAF, 3);
    old.node_append_kv(0, 0, bytes("k1"), bytes("v1"));
    old.node_append_kv(1, 0, bytes("k2"), bytes("v2"));
    old.node_append_kv(2, 0, bytes("k3"), bytes("v3"));

    BNode new_node;
    new_node.set_header(BNODE_LEAF, 2);
    node_append_range(new_node, old, 0, 1, 2); // copy k2, k3

    EXPECT_EQ(str(new_node.get_key(0)), "k2");
    EXPECT_EQ(str(new_node.get_key(1)), "k3");
}

TEST(NodeAppendRange, WhenNIsZeroThenNothingIsCopied) {
    BNode old;
    old.set_header(BNODE_LEAF, 1);
    old.node_append_kv(0, 0, bytes("k1"), bytes("v1"));

    BNode new_node;
    new_node.set_header(BNODE_LEAF, 0);
    node_append_range(new_node, old, 0, 0, 0);

    EXPECT_EQ(new_node.nbytes(), new_node.kv_base_offset());
}

// ============================================================================
// leaf_insert
// ============================================================================
TEST(LeafInsert, WhenInsertingAtBeginningThenNewKeyComesFirst) {
    BNode old;
    old.set_header(BNODE_LEAF, 2);
    old.node_append_kv(0, 0, bytes("k2"), bytes("v2"));
    old.node_append_kv(1, 0, bytes("k3"), bytes("v3"));

    BNode new_node;
    leaf_insert(new_node, old, 0, bytes("k1"), bytes("v1"));

    EXPECT_EQ(new_node.nkeys(), 3);
    EXPECT_EQ(str(new_node.get_key(0)), "k1");
    EXPECT_EQ(str(new_node.get_val(0)), "v1");
    EXPECT_EQ(str(new_node.get_key(1)), "k2");
    EXPECT_EQ(str(new_node.get_key(2)), "k3");
}

TEST(LeafInsert, WhenInsertingInMiddleThenExistingKeysShiftAround) {
    BNode old;
    old.set_header(BNODE_LEAF, 2);
    old.node_append_kv(0, 0, bytes("k1"), bytes("v1"));
    old.node_append_kv(1, 0, bytes("k3"), bytes("v3"));

    BNode new_node;
    leaf_insert(new_node, old, 1, bytes("k2"), bytes("v2"));

    EXPECT_EQ(new_node.nkeys(), 3);
    EXPECT_EQ(str(new_node.get_key(0)), "k1");
    EXPECT_EQ(str(new_node.get_key(1)), "k2");
    EXPECT_EQ(str(new_node.get_val(1)), "v2");
    EXPECT_EQ(str(new_node.get_key(2)), "k3");
}

TEST(LeafInsert, WhenInsertingAtEndThenNewKeyComesLast) {
    BNode old;
    old.set_header(BNODE_LEAF, 2);
    old.node_append_kv(0, 0, bytes("k1"), bytes("v1"));
    old.node_append_kv(1, 0, bytes("k2"), bytes("v2"));

    BNode new_node;
    leaf_insert(new_node, old, 2, bytes("k3"), bytes("v3"));

    EXPECT_EQ(new_node.nkeys(), 3);
    EXPECT_EQ(str(new_node.get_key(2)), "k3");
    EXPECT_EQ(str(new_node.get_val(2)), "v3");
}

TEST(LeafInsert, WhenInsertingIntoEmptyNodeThenSingleKeyIsStored) {
    BNode old;
    old.set_header(BNODE_LEAF, 0);

    BNode new_node;
    leaf_insert(new_node, old, 0, bytes("k1"), bytes("v1"));

    EXPECT_EQ(new_node.nkeys(), 1);
    EXPECT_EQ(str(new_node.get_key(0)), "k1");
    EXPECT_EQ(str(new_node.get_val(0)), "v1");
}

// ============================================================================
// leaf_update
// ============================================================================
TEST(LeafUpdate, WhenUpdatingMiddleKeyThenOnlyItsValueChanges) {
    BNode old;
    old.set_header(BNODE_LEAF, 3);
    old.node_append_kv(0, 0, bytes("k1"), bytes("v1"));
    old.node_append_kv(1, 0, bytes("k2"), bytes("v2"));
    old.node_append_kv(2, 0, bytes("k3"), bytes("v3"));

    BNode new_node;
    leaf_update(new_node, old, 1, bytes("k2"), bytes("updated"));

    EXPECT_EQ(new_node.nkeys(), 3);
    EXPECT_EQ(str(new_node.get_key(0)), "k1");
    EXPECT_EQ(str(new_node.get_val(0)), "v1");
    EXPECT_EQ(str(new_node.get_key(1)), "k2");
    EXPECT_EQ(str(new_node.get_val(1)), "updated");
    EXPECT_EQ(str(new_node.get_key(2)), "k3");
    EXPECT_EQ(str(new_node.get_val(2)), "v3");
}

TEST(LeafUpdate, WhenUpdatingFirstKeyThenValueChangesAndOrderPreserved) {
    BNode old;
    old.set_header(BNODE_LEAF, 2);
    old.node_append_kv(0, 0, bytes("k1"), bytes("v1"));
    old.node_append_kv(1, 0, bytes("k2"), bytes("v2"));

    BNode new_node;
    leaf_update(new_node, old, 0, bytes("k1"), bytes("updated"));

    EXPECT_EQ(new_node.nkeys(), 2);
    EXPECT_EQ(str(new_node.get_val(0)), "updated");
    EXPECT_EQ(str(new_node.get_key(1)), "k2");
}

TEST(LeafUpdate, WhenUpdatingLastKeyThenValueChangesAndOrderPreserved) {
    BNode old;
    old.set_header(BNODE_LEAF, 2);
    old.node_append_kv(0, 0, bytes("k1"), bytes("v1"));
    old.node_append_kv(1, 0, bytes("k2"), bytes("v2"));

    BNode new_node;
    leaf_update(new_node, old, 1, bytes("k2"), bytes("updated"));

    EXPECT_EQ(new_node.nkeys(), 2);
    EXPECT_EQ(str(new_node.get_key(0)), "k1");
    EXPECT_EQ(str(new_node.get_val(1)), "updated");
}

// ============================================================================
// node_lookup_le
// ============================================================================
TEST(NodeLookupLE, WhenKeyMatchesExactlyThenIndexOfMatchIsReturned) {
    BNode node;
    node.set_header(BNODE_LEAF, 3);
    node.node_append_kv(0, 0, bytes("k1"), bytes("v1"));
    node.node_append_kv(1, 0, bytes("k3"), bytes("v3"));
    node.node_append_kv(2, 0, bytes("k5"), bytes("v5"));

    EXPECT_EQ(node_lookup_le(node, bytes("k3")), 1);
}

TEST(NodeLookupLE, WhenKeyFallsBetweenEntriesThenLowerIndexIsReturned) {
    BNode node;
    node.set_header(BNODE_LEAF, 3);
    node.node_append_kv(0, 0, bytes("k1"), bytes("v1"));
    node.node_append_kv(1, 0, bytes("k3"), bytes("v3"));
    node.node_append_kv(2, 0, bytes("k5"), bytes("v5"));

    EXPECT_EQ(node_lookup_le(node, bytes("k4")), 1);
}

TEST(NodeLookupLE, WhenKeyIsGreaterThanAllEntriesThenLastIndexIsReturned) {
    BNode node;
    node.set_header(BNODE_LEAF, 3);
    node.node_append_kv(0, 0, bytes("k1"), bytes("v1"));
    node.node_append_kv(1, 0, bytes("k3"), bytes("v3"));
    node.node_append_kv(2, 0, bytes("k5"), bytes("v5"));

    EXPECT_EQ(node_lookup_le(node, bytes("k9")), 2);
}

TEST(NodeLookupLE, WhenKeyIsLessThanAllEntriesThenNegativeOneIsReturned) {
    BNode node;
    node.set_header(BNODE_LEAF, 3);
    node.node_append_kv(0, 0, bytes("k1"), bytes("v1"));
    node.node_append_kv(1, 0, bytes("k3"), bytes("v3"));
    node.node_append_kv(2, 0, bytes("k5"), bytes("v5"));

    EXPECT_EQ(node_lookup_le(node, bytes("k0")), -1);
}

TEST(NodeLookupLE, WhenNodeIsEmptyThenNegativeOneIsReturned) {
    BNode node;
    node.set_header(BNODE_LEAF, 0);

    EXPECT_EQ(node_lookup_le(node, bytes("k1")), -1);
}

TEST(NodeLookupLE, WhenManyKeysThenEveryPositionIsFoundCorrectly) {
    // Exercises binary search across even/odd counts and both halves.
    BNode node;
    const int n = 16;
    node.set_header(BNODE_LEAF, n);
    for (int i = 0; i < n; ++i) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "k%02d", i * 2); // k00, k02, k04, ...
        node.node_append_kv(static_cast<uint16_t>(i), 0, bytes(buf), bytes("v"));
    }

    for (int i = 0; i < n; ++i) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "k%02d", i * 2);
        EXPECT_EQ(node_lookup_le(node, bytes(buf)), i) << "exact match at " << i;
    }
    // Odd keys fall strictly between two even keys.
    EXPECT_EQ(node_lookup_le(node, bytes("k01")), 0);
    EXPECT_EQ(node_lookup_le(node, bytes("k15")), 7);
    EXPECT_EQ(node_lookup_le(node, bytes("k29")), 14);
    EXPECT_EQ(node_lookup_le(node, bytes("k99")), n - 1);
}

// ============================================================================
// Insert or update after a key lookup
// ============================================================================
TEST(LeafInsertOrUpdate, WhenKeyExistsThenUpdatePath) {
    BNode node;
    node.set_header(BNODE_LEAF, 2);
    node.node_append_kv(0, 0, bytes("k1"), bytes("v1"));
    node.node_append_kv(1, 0, bytes("k3"), bytes("v3"));

    int64_t idx = node_lookup_le(node, bytes("k3"));
    ASSERT_GE(idx, 0);

    BNode new_node;
    if (node.get_key(static_cast<uint16_t>(idx)) == bytes("k3")) {
        leaf_update(new_node, node, static_cast<uint16_t>(idx), bytes("k3"), bytes("updated"));
    } else {
        FAIL() << "expected key match";
    }

    EXPECT_EQ(new_node.nkeys(), 2);
    EXPECT_EQ(str(new_node.get_val(1)), "updated");
}

TEST(LeafInsertOrUpdate, WhenKeyDoesNotExistThenInsertPath) {
    BNode node;
    node.set_header(BNODE_LEAF, 2);
    node.node_append_kv(0, 0, bytes("k1"), bytes("v1"));
    node.node_append_kv(1, 0, bytes("k3"), bytes("v3"));

    int64_t idx = node_lookup_le(node, bytes("k2"));
    ASSERT_GE(idx, 0);
    ASSERT_NE(node.get_key(static_cast<uint16_t>(idx)), bytes("k2"));

    BNode new_node;
    leaf_insert(new_node, node, static_cast<uint16_t>(idx) + 1, bytes("k2"), bytes("v2"));

    EXPECT_EQ(new_node.nkeys(), 3);
    EXPECT_EQ(str(new_node.get_key(0)), "k1");
    EXPECT_EQ(str(new_node.get_key(1)), "k2");
    EXPECT_EQ(str(new_node.get_key(2)), "k3");
}

// ============================================================================
// node_split
// ============================================================================
namespace {
    // A key that sorts by `idx`: a 4-byte big-endian index prefix followed by
    // filler bytes out to `len` total (len must be >= 4).
    std::vector<uint8_t> indexed_key(uint32_t idx, size_t len) {
        std::vector<uint8_t> k(len, 'x');
        k[0] = static_cast<uint8_t>(idx >> 24);
        k[1] = static_cast<uint8_t>(idx >> 16);
        k[2] = static_cast<uint8_t>(idx >> 8);
        k[3] = static_cast<uint8_t>(idx);
        return k;
    }

    // Builds a leaf BNode (possibly oversized) from a list of (key_len,
    // val_len) entry sizes. Keys are assigned in sorted order via
    // indexed_key, so ordering invariants hold.
    BNode build_leaf(const std::vector<std::pair<size_t, size_t>>& entries) {
        size_t n = entries.size();
        size_t used = 4 + n * 10;
        for (const auto& [klen, vlen] : entries) {
            used += 4 + klen + vlen;
        }

        BNode node(used); // exactly large enough to hold the oversized node
        node.set_header(BNODE_LEAF, static_cast<uint16_t>(n));
        for (uint16_t i = 0; i < n; ++i) {
            auto [klen, vlen] = entries[i];
            node.node_append_kv(i, 0, indexed_key(i, klen), std::vector<uint8_t>(vlen, 'v'));
        }
        return node;
    }

    // Checks the invariants node_split must always uphold: both halves fit
    // within a page, together they contain exactly old's keys, and the
    // relative order (and ptrs) are preserved.
    void expect_valid_split(const BNode& old, const BNode& left, const BNode& right) {
        EXPECT_LE(left.nbytes(), BTREE_PAGE_SIZE);
        EXPECT_LE(right.nbytes(), BTREE_PAGE_SIZE);
        ASSERT_GE(left.nkeys(), 1);
        ASSERT_GE(right.nkeys(), 1);
        ASSERT_EQ(static_cast<uint32_t>(left.nkeys()) + right.nkeys(), old.nkeys());

        for (uint16_t i = 0; i < left.nkeys(); ++i) {
            EXPECT_EQ(left.get_key(i), old.get_key(i));
            EXPECT_EQ(left.get_val(i), old.get_val(i));
            EXPECT_EQ(left.get_ptr(i), old.get_ptr(i));
        }
        for (uint16_t i = 0; i < right.nkeys(); ++i) {
            uint16_t old_idx = static_cast<uint16_t>(left.nkeys() + i);
            EXPECT_EQ(right.get_key(i), old.get_key(old_idx));
            EXPECT_EQ(right.get_val(i), old.get_val(old_idx));
            EXPECT_EQ(right.get_ptr(i), old.get_ptr(old_idx));
        }
    }
}

TEST(NodeSplit, WhenEntriesAreUniformThenBothHalvesFitAndKeysArePreserved) {
    // 20 entries of 204 bytes each overflow a single page (~4284 bytes).
    BNode old = build_leaf(std::vector<std::pair<size_t, size_t>>(20, {100, 100}));
    ASSERT_GT(old.nbytes(), BTREE_PAGE_SIZE);

    BNode left(BTREE_PAGE_SIZE), right(BTREE_PAGE_SIZE);
    node_split(left, right, old);

    expect_valid_split(old, left, right);
    EXPECT_EQ(left.nkeys(), 10);
    EXPECT_EQ(right.nkeys(), 10);
}

TEST(NodeSplit, WhenBigKeysClusterOnLeftThenGuessShrinksToFitLeft) {
    // 3 max-size entries (2014B each) followed by 3 small ones. The naive
    // half-point guess (nleft=3) includes all 3 big entries and overflows a
    // page, so node_split must shrink nleft before it fits.
    BNode old = build_leaf({
        {BTREE_MAX_KEY_SIZE, BTREE_MAX_VAL_SIZE},
        {BTREE_MAX_KEY_SIZE, BTREE_MAX_VAL_SIZE},
        {BTREE_MAX_KEY_SIZE, BTREE_MAX_VAL_SIZE},
        {10, 10},
        {10, 10},
        {10, 10},
    });
    ASSERT_GT(old.nbytes(), BTREE_PAGE_SIZE);

    BNode left(BTREE_PAGE_SIZE), right(BTREE_PAGE_SIZE);
    node_split(left, right, old);

    expect_valid_split(old, left, right);
    // Guess (3) had to shrink: left ends up with only 2 of the 3 big keys.
    EXPECT_EQ(left.nkeys(), 2);
    EXPECT_EQ(right.nkeys(), 4);
}

TEST(NodeSplit, WhenBigKeysClusterOnRightThenGuessGrowsToFitRight) {
    // Mirror of the above: 3 small entries followed by 3 max-size ones. The
    // naive guess (nleft=3) leaves all 3 big entries on the right, which
    // overflows a page, so node_split must grow nleft to absorb one of them.
    BNode old = build_leaf({
        {10, 10},
        {10, 10},
        {10, 10},
        {BTREE_MAX_KEY_SIZE, BTREE_MAX_VAL_SIZE},
        {BTREE_MAX_KEY_SIZE, BTREE_MAX_VAL_SIZE},
        {BTREE_MAX_KEY_SIZE, BTREE_MAX_VAL_SIZE},
    });
    ASSERT_GT(old.nbytes(), BTREE_PAGE_SIZE);

    BNode left(BTREE_PAGE_SIZE), right(BTREE_PAGE_SIZE);
    node_split(left, right, old);

    expect_valid_split(old, left, right);
    // Guess (3) had to grow: left absorbs 1 of the 3 big keys, leaving only
    // 2 (which is exactly the max that fit in one page together) on the right.
    EXPECT_EQ(left.nkeys(), 4);
    EXPECT_EQ(right.nkeys(), 2);
}

TEST(NodeSplit, WhenTwoMaxSizeEntriesThenTheyJustFitOnePageTogether) {
    BNode node = build_leaf({
        {BTREE_MAX_KEY_SIZE, BTREE_MAX_VAL_SIZE},
        {BTREE_MAX_KEY_SIZE, BTREE_MAX_VAL_SIZE},
    });
    EXPECT_LE(node.nbytes(), BTREE_PAGE_SIZE);
}

TEST(NodeSplit, WhenSplittingInternalNodeThenChildPointersArePreserved) {
    BNode old(BTREE_PAGE_SIZE * 2);
    const int n = 30;
    old.set_header(BNODE_NODE, n);
    for (int i = 0; i < n; ++i) {
        old.node_append_kv(static_cast<uint16_t>(i), 1000 + i, indexed_key(i, 150), {});
    }
    ASSERT_GT(old.nbytes(), BTREE_PAGE_SIZE);

    BNode left(BTREE_PAGE_SIZE), right(BTREE_PAGE_SIZE);
    node_split(left, right, old);

    expect_valid_split(old, left, right);
    EXPECT_EQ(left.btype(), BNODE_NODE);
    EXPECT_EQ(right.btype(), BNODE_NODE);
}

TEST(NodeSplit, WhenOldHasFewerThanTwoKeysThenAsserts) {
    BNode old;
    old.set_header(BNODE_LEAF, 1);
    old.node_append_kv(0, 0, bytes("k1"), bytes("v1"));

    BNode left(BTREE_PAGE_SIZE), right(BTREE_PAGE_SIZE);
    EXPECT_DEATH(node_split(left, right, old), "");
}

// ============================================================================
// node_split_if_needed
// ============================================================================
TEST(NodeSplitIfNeeded, WhenNodeFitsThenReturnsSingleUnchangedNode) {
    BNode node;
    node.set_header(BNODE_LEAF, 2);
    node.node_append_kv(0, 0, bytes("k1"), bytes("v1"));
    node.node_append_kv(1, 0, bytes("k3"), bytes("v3"));

    std::vector<BNode> result = node_split_if_needed(node);

    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].nkeys(), 2);
    EXPECT_EQ(str(result[0].get_key(1)), "k3");
}

TEST(NodeSplitIfNeeded, WhenNodeExceedsPageSizeThenReturnsSplitPair) {
    BNode old = build_leaf(std::vector<std::pair<size_t, size_t>>(20, {100, 100}));
    ASSERT_GT(old.nbytes(), BTREE_PAGE_SIZE);

    std::vector<BNode> result = node_split_if_needed(old);

    ASSERT_EQ(result.size(), 2u);
    expect_valid_split(old, result[0], result[1]);
}

// ============================================================================
// leaf_delete
// ============================================================================
TEST(LeafDelete, WhenDeletingMiddleKeyThenRemainingKeysShift) {
    BNode old;
    old.set_header(BNODE_LEAF, 3);
    old.node_append_kv(0, 0, bytes("k1"), bytes("v1"));
    old.node_append_kv(1, 0, bytes("k2"), bytes("v2"));
    old.node_append_kv(2, 0, bytes("k3"), bytes("v3"));

    BNode new_node;
    leaf_delete(new_node, old, 1);

    EXPECT_EQ(new_node.nkeys(), 2);
    EXPECT_EQ(str(new_node.get_key(0)), "k1");
    EXPECT_EQ(str(new_node.get_key(1)), "k3");
}

TEST(LeafDelete, WhenDeletingFirstKeyThenOnlyLaterKeysRemain) {
    BNode old;
    old.set_header(BNODE_LEAF, 2);
    old.node_append_kv(0, 0, bytes("k1"), bytes("v1"));
    old.node_append_kv(1, 0, bytes("k2"), bytes("v2"));

    BNode new_node;
    leaf_delete(new_node, old, 0);

    EXPECT_EQ(new_node.nkeys(), 1);
    EXPECT_EQ(str(new_node.get_key(0)), "k2");
}

TEST(LeafDelete, WhenDeletingLastKeyThenOnlyPrecedingKeysRemain) {
    BNode old;
    old.set_header(BNODE_LEAF, 2);
    old.node_append_kv(0, 0, bytes("k1"), bytes("v1"));
    old.node_append_kv(1, 0, bytes("k2"), bytes("v2"));

    BNode new_node;
    leaf_delete(new_node, old, 1);

    EXPECT_EQ(new_node.nkeys(), 1);
    EXPECT_EQ(str(new_node.get_key(0)), "k1");
}

TEST(LeafDelete, WhenDeletingOnlyKeyThenResultIsEmpty) {
    BNode old;
    old.set_header(BNODE_LEAF, 1);
    old.node_append_kv(0, 0, bytes("k1"), bytes("v1"));

    BNode new_node;
    leaf_delete(new_node, old, 0);

    EXPECT_EQ(new_node.nkeys(), 0);
}

// ============================================================================
// node_merge
// ============================================================================
TEST(NodeMerge, WhenMergingTwoLeafNodesThenAllKeysArePreservedInOrder) {
    BNode left;
    left.set_header(BNODE_LEAF, 2);
    left.node_append_kv(0, 0, bytes("k1"), bytes("v1"));
    left.node_append_kv(1, 0, bytes("k2"), bytes("v2"));

    BNode right;
    right.set_header(BNODE_LEAF, 2);
    right.node_append_kv(0, 0, bytes("k3"), bytes("v3"));
    right.node_append_kv(1, 0, bytes("k4"), bytes("v4"));

    BNode merged;
    node_merge(merged, left, right);

    ASSERT_EQ(merged.nkeys(), 4);
    EXPECT_EQ(merged.btype(), BNODE_LEAF);
    EXPECT_EQ(str(merged.get_key(0)), "k1");
    EXPECT_EQ(str(merged.get_key(1)), "k2");
    EXPECT_EQ(str(merged.get_key(2)), "k3");
    EXPECT_EQ(str(merged.get_key(3)), "k4");
    EXPECT_EQ(str(merged.get_val(3)), "v4");
}

TEST(NodeMerge, WhenMergingInternalNodesThenChildPointersArePreserved) {
    BNode left(BTREE_PAGE_SIZE);
    left.set_header(BNODE_NODE, 1);
    left.node_append_kv(0, 100, bytes("a"), {});

    BNode right(BTREE_PAGE_SIZE);
    right.set_header(BNODE_NODE, 1);
    right.node_append_kv(0, 200, bytes("m"), {});

    BNode merged;
    node_merge(merged, left, right);

    ASSERT_EQ(merged.nkeys(), 2);
    EXPECT_EQ(merged.btype(), BNODE_NODE);
    EXPECT_EQ(merged.get_ptr(0), 100u);
    EXPECT_EQ(str(merged.get_key(0)), "a");
    EXPECT_EQ(merged.get_ptr(1), 200u);
    EXPECT_EQ(str(merged.get_key(1)), "m");
}

// ============================================================================
// node_replace_2_child
// ============================================================================
TEST(NodeReplace2Child, WhenReplacingTwoAdjacentChildrenThenSingleLinkReplacesThem) {
    BNode old(BTREE_PAGE_SIZE);
    old.set_header(BNODE_NODE, 3);
    old.node_append_kv(0, 10, bytes("a"), {});
    old.node_append_kv(1, 20, bytes("m"), {});
    old.node_append_kv(2, 30, bytes("z"), {});

    BNode new_node;
    node_replace_2_child(new_node, old, 0, 99, bytes("a"));

    ASSERT_EQ(new_node.nkeys(), 2);
    EXPECT_EQ(new_node.get_ptr(0), 99u);
    EXPECT_EQ(str(new_node.get_key(0)), "a");
    EXPECT_EQ(new_node.get_ptr(1), 30u);
    EXPECT_EQ(str(new_node.get_key(1)), "z");
}

TEST(NodeReplace2Child, WhenReplacingAtEndThenPrecedingChildrenAreUnchanged) {
    BNode old(BTREE_PAGE_SIZE);
    old.set_header(BNODE_NODE, 3);
    old.node_append_kv(0, 10, bytes("a"), {});
    old.node_append_kv(1, 20, bytes("m"), {});
    old.node_append_kv(2, 30, bytes("z"), {});

    BNode new_node;
    node_replace_2_child(new_node, old, 1, 99, bytes("m"));

    ASSERT_EQ(new_node.nkeys(), 2);
    EXPECT_EQ(new_node.get_ptr(0), 10u);
    EXPECT_EQ(str(new_node.get_key(0)), "a");
    EXPECT_EQ(new_node.get_ptr(1), 99u);
    EXPECT_EQ(str(new_node.get_key(1)), "m");
}

// ============================================================================
// tree_insert: leaf nodes
// ============================================================================
TEST(TreeInsert, WhenKeyIsNewThenLeafGrowsWithKeyInSortedPosition) {
    BNode leaf;
    leaf.set_header(BNODE_LEAF, 2);
    leaf.node_append_kv(0, 0, bytes("k1"), bytes("v1"));
    leaf.node_append_kv(1, 0, bytes("k3"), bytes("v3"));

    BTree tree;
    BNode result = tree_insert(tree, leaf, bytes("k2"), bytes("v2"));

    EXPECT_EQ(result.nkeys(), 3);
    EXPECT_EQ(str(result.get_key(1)), "k2");
    EXPECT_EQ(str(result.get_val(1)), "v2");
}

TEST(TreeInsert, WhenKeyExistsThenValueIsReplacedInPlace) {
    BNode leaf;
    leaf.set_header(BNODE_LEAF, 2);
    leaf.node_append_kv(0, 0, bytes("k1"), bytes("v1"));
    leaf.node_append_kv(1, 0, bytes("k3"), bytes("v3"));

    BTree tree;
    BNode result = tree_insert(tree, leaf, bytes("k3"), bytes("updated"));

    EXPECT_EQ(result.nkeys(), 2);
    EXPECT_EQ(str(result.get_key(1)), "k3");
    EXPECT_EQ(str(result.get_val(1)), "updated");
}

TEST(TreeInsert, WhenInternalNodeFirstKeyExceedsSearchKeyThenAsserts) {
    BNode parent(BTREE_PAGE_SIZE);
    parent.set_header(BNODE_NODE, 1);
    parent.node_append_kv(0, 1, bytes("k5"), {});

    BTree tree;
    EXPECT_DEATH(tree_insert(tree, parent, bytes("k1"), bytes("v1")), "");
}

// ============================================================================
// tree_delete: leaf nodes
// ============================================================================
TEST(TreeDelete, WhenKeyExistsInLeafThenItIsRemoved) {
    BNode leaf;
    leaf.set_header(BNODE_LEAF, 3);
    leaf.node_append_kv(0, 0, bytes("k1"), bytes("v1"));
    leaf.node_append_kv(1, 0, bytes("k2"), bytes("v2"));
    leaf.node_append_kv(2, 0, bytes("k3"), bytes("v3"));

    BTree tree;
    BNode result = tree_delete(tree, leaf, bytes("k2"));

    ASSERT_FALSE(result.data.empty());
    EXPECT_EQ(result.nkeys(), 2);
    EXPECT_EQ(str(result.get_key(0)), "k1");
    EXPECT_EQ(str(result.get_key(1)), "k3");
}

TEST(TreeDelete, WhenKeyDoesNotExistInLeafThenEmptyNodeIsReturned) {
    BNode leaf;
    leaf.set_header(BNODE_LEAF, 2);
    leaf.node_append_kv(0, 0, bytes("k1"), bytes("v1"));
    leaf.node_append_kv(1, 0, bytes("k3"), bytes("v3"));

    BTree tree;
    BNode result = tree_delete(tree, leaf, bytes("k2"));

    EXPECT_TRUE(result.data.empty());
}

TEST(TreeDelete, WhenInternalNodeFirstKeyExceedsSearchKeyThenAsserts) {
    BNode parent(BTREE_PAGE_SIZE);
    parent.set_header(BNODE_NODE, 1);
    parent.node_append_kv(0, 1, bytes("k5"), {});

    BTree tree;
    EXPECT_DEATH(tree_delete(tree, parent, bytes("k1")), "");
}

// ============================================================================
// tree_insert / node_insert: internal nodes, via an in-memory PageManager
// ============================================================================
namespace {
    class InMemoryPageManager : public IPageManager {
    public:
        BNode get(uint64_t ptr) const override { return pages_.at(ptr); }

        uint64_t new_page(const BNode& node) override {
            uint64_t ptr = next_ptr_++;
            pages_[ptr] = node;
            return ptr;
        }

        void del(uint64_t ptr) override { pages_.erase(ptr); }

        bool has_page(uint64_t ptr) const { return pages_.count(ptr) > 0; }

    private:
        std::unordered_map<uint64_t, BNode> pages_;
        uint64_t next_ptr_ = 1;
    };

    BNode make_leaf(uint32_t start_idx, uint16_t count, size_t klen, size_t vlen) {
        BNode node;
        node.set_header(BNODE_LEAF, count);
        for (uint16_t i = 0; i < count; ++i) {
            node.node_append_kv(i, 0, indexed_key(start_idx + i, klen), std::vector<uint8_t>(vlen, 'v'));
        }
        return node;
    }
}

TEST(TreeInsert, WhenInsertingIntoInternalNodeThenTargetChildIsUpdated) {
    InMemoryPageManager pages;
    BNode child0 = make_leaf(0, 3, 20, 20);
    BNode child1 = make_leaf(1000, 3, 20, 20);
    uint64_t ptr0 = pages.new_page(child0);
    uint64_t ptr1 = pages.new_page(child1);

    BNode parent(BTREE_PAGE_SIZE);
    parent.set_header(BNODE_NODE, 2);
    parent.node_append_kv(0, ptr0, child0.get_key(0), {});
    parent.node_append_kv(1, ptr1, child1.get_key(0), {});

    BTree tree{0, &pages};
    BNode result = tree_insert(tree, parent, indexed_key(1, 20), bytes("updated"));

    ASSERT_EQ(result.nkeys(), 2);
    EXPECT_FALSE(pages.has_page(ptr0));

    uint64_t new_ptr0 = result.get_ptr(0);
    EXPECT_NE(new_ptr0, ptr0);
    BNode new_child0 = pages.get(new_ptr0);
    EXPECT_EQ(new_child0.nkeys(), 3);
    EXPECT_EQ(str(new_child0.get_val(1)), "updated");

    EXPECT_EQ(result.get_ptr(1), ptr1);
    EXPECT_EQ(result.get_key(1), child1.get_key(0));
}

TEST(TreeInsert, WhenChildOverflowsThenItSplitsAndParentGrowsByOneKey) {
    InMemoryPageManager pages;
    BNode child0 = make_leaf(0, 19, 100, 100);
    BNode child1 = make_leaf(1000, 3, 20, 20);
    uint64_t ptr0 = pages.new_page(child0);
    uint64_t ptr1 = pages.new_page(child1);

    BNode parent(BTREE_PAGE_SIZE);
    parent.set_header(BNODE_NODE, 2);
    parent.node_append_kv(0, ptr0, child0.get_key(0), {});
    parent.node_append_kv(1, ptr1, child1.get_key(0), {});

    BTree tree{0, &pages};
    BNode result = tree_insert(tree, parent, indexed_key(19, 100), std::vector<uint8_t>(100, 'v'));

    ASSERT_EQ(result.nkeys(), 3);
    EXPECT_FALSE(pages.has_page(ptr0));

    BNode split_left = pages.get(result.get_ptr(0));
    BNode split_right = pages.get(result.get_ptr(1));
    EXPECT_EQ(split_left.nkeys(), 10);
    EXPECT_EQ(split_right.nkeys(), 10);
    EXPECT_EQ(result.get_key(0), child0.get_key(0));
    EXPECT_EQ(result.get_key(1), split_right.get_key(0));

    EXPECT_EQ(result.get_ptr(2), ptr1);
    EXPECT_EQ(result.get_key(2), child1.get_key(0));
}

// ============================================================================
// should_merge
// ============================================================================
TEST(ShouldMerge, WhenUpdatedNodeIsLargeThenNoMergeIsSuggested) {
    InMemoryPageManager pages;
    BNode sibling = make_leaf(1000, 3, 20, 20);
    uint64_t ptr = pages.new_page(sibling);

    BNode parent(BTREE_PAGE_SIZE);
    parent.set_header(BNODE_NODE, 2);
    parent.node_append_kv(0, 1, bytes("a"), {});
    parent.node_append_kv(1, ptr, sibling.get_key(0), {});

    // Large enough on its own (>1/4 page) that a merge should never be
    // suggested, regardless of siblings.
    BNode updated = build_leaf(std::vector<std::pair<size_t, size_t>>(12, {100, 100}));
    ASSERT_GT(updated.nbytes(), BTREE_PAGE_SIZE / 4);

    BTree tree{0, &pages};
    auto [direction, merge_sibling] = should_merge(tree, parent, 0, updated);

    EXPECT_EQ(direction, MergeDirection::None);
}

TEST(ShouldMerge, WhenLeftSiblingFitsThenMergeWithLeftIsSuggested) {
    InMemoryPageManager pages;
    BNode left_sibling = make_leaf(0, 1, 10, 10);
    BNode right_sibling = make_leaf(1000, 1, 10, 10);
    uint64_t left_ptr = pages.new_page(left_sibling);
    uint64_t right_ptr = pages.new_page(right_sibling);

    BNode parent(BTREE_PAGE_SIZE);
    parent.set_header(BNODE_NODE, 2);
    parent.node_append_kv(0, left_ptr, left_sibling.get_key(0), {});
    parent.node_append_kv(1, right_ptr, right_sibling.get_key(0), {});

    BNode updated = make_leaf(500, 1, 10, 10);
    ASSERT_LE(updated.nbytes(), BTREE_PAGE_SIZE / 4);

    BTree tree{0, &pages};
    auto [direction, merge_sibling] = should_merge(tree, parent, 1, updated);

    EXPECT_EQ(direction, MergeDirection::Left);
    EXPECT_EQ(merge_sibling.nkeys(), left_sibling.nkeys());
    EXPECT_EQ(merge_sibling.get_key(0), left_sibling.get_key(0));
}

TEST(ShouldMerge, WhenRightSiblingFitsThenMergeWithRightIsSuggested) {
    InMemoryPageManager pages;
    BNode right_sibling = make_leaf(1000, 1, 10, 10);
    uint64_t right_ptr = pages.new_page(right_sibling);

    BNode parent(BTREE_PAGE_SIZE);
    parent.set_header(BNODE_NODE, 2);
    parent.node_append_kv(0, 1, bytes("a"), {});
    parent.node_append_kv(1, right_ptr, right_sibling.get_key(0), {});

    BNode updated = make_leaf(0, 1, 10, 10);
    ASSERT_LE(updated.nbytes(), BTREE_PAGE_SIZE / 4);

    BTree tree{0, &pages};
    // idx = 0: no left sibling, so should fall through to the right one.
    auto [direction, merge_sibling] = should_merge(tree, parent, 0, updated);

    EXPECT_EQ(direction, MergeDirection::Right);
    EXPECT_EQ(merge_sibling.nkeys(), right_sibling.nkeys());
    EXPECT_EQ(merge_sibling.get_key(0), right_sibling.get_key(0));
}

TEST(ShouldMerge, WhenNoSiblingsExistThenNoMergeIsSuggested) {
    InMemoryPageManager pages;
    BNode updated = make_leaf(0, 1, 10, 10);
    ASSERT_LE(updated.nbytes(), BTREE_PAGE_SIZE / 4);

    BNode parent(BTREE_PAGE_SIZE);
    parent.set_header(BNODE_NODE, 1);
    parent.node_append_kv(0, 1, bytes("a"), {});

    BTree tree{0, &pages};
    auto [direction, merge_sibling] = should_merge(tree, parent, 0, updated);

    EXPECT_EQ(direction, MergeDirection::None);
}

TEST(ShouldMerge, WhenLeftSiblingIsTooLargeThenRightSiblingIsConsideredInstead) {
    InMemoryPageManager pages;
    // Left sibling is nearly a full page on its own, so merging it with
    // `updated` would overflow a page; the small right sibling still fits.
    BNode left_sibling = make_leaf(0, 19, 100, 100);
    BNode right_sibling = make_leaf(1000, 1, 10, 10);
    uint64_t left_ptr = pages.new_page(left_sibling);
    uint64_t right_ptr = pages.new_page(right_sibling);

    // 3 children so idx=1 has both a left (idx0) and a right (idx2) sibling
    // to consider.
    BNode parent(BTREE_PAGE_SIZE);
    parent.set_header(BNODE_NODE, 3);
    parent.node_append_kv(0, left_ptr, left_sibling.get_key(0), {});
    parent.node_append_kv(1, 999, bytes("mid"), {}); // placeholder; not fetched by should_merge
    parent.node_append_kv(2, right_ptr, right_sibling.get_key(0), {});

    BNode updated = make_leaf(500, 1, 10, 10);
    ASSERT_LE(updated.nbytes(), BTREE_PAGE_SIZE / 4);
    ASSERT_GT(static_cast<uint32_t>(left_sibling.nbytes()) + updated.nbytes(), BTREE_PAGE_SIZE);

    BTree tree{0, &pages};
    auto [direction, merge_sibling] = should_merge(tree, parent, 1, updated);

    EXPECT_EQ(direction, MergeDirection::Right);
    EXPECT_EQ(merge_sibling.get_key(0), right_sibling.get_key(0));
}

// ============================================================================
// tree_delete / node_delete: internal nodes, via an in-memory PageManager
// ============================================================================
TEST(NodeDelete, WhenDeletingFromInternalNodeThenTargetChildIsUpdatedWithoutMerging) {
    InMemoryPageManager pages;
    // child0 stays well above the 1/4-page merge threshold even after losing
    // one entry, so should_merge must never even be triggered.
    BNode child0 = make_leaf(0, 19, 100, 100);
    BNode child1 = make_leaf(1000, 3, 20, 20);
    uint64_t ptr0 = pages.new_page(child0);
    uint64_t ptr1 = pages.new_page(child1);

    BNode parent(BTREE_PAGE_SIZE);
    parent.set_header(BNODE_NODE, 2);
    parent.node_append_kv(0, ptr0, child0.get_key(0), {});
    parent.node_append_kv(1, ptr1, child1.get_key(0), {});

    BTree tree{0, &pages};
    BNode result = node_delete(tree, parent, 0, child0.get_key(5));

    ASSERT_FALSE(result.data.empty());
    ASSERT_EQ(result.nkeys(), 2); // no merge happened
    EXPECT_FALSE(pages.has_page(ptr0));

    BNode new_child0 = pages.get(result.get_ptr(0));
    EXPECT_EQ(new_child0.nkeys(), 18);
    EXPECT_EQ(result.get_ptr(1), ptr1); // sibling untouched
    EXPECT_EQ(result.get_key(1), child1.get_key(0));
}

TEST(NodeDelete, WhenChildBecomesSmallThenItMergesWithSiblingAndParentShrinks) {
    InMemoryPageManager pages;
    BNode child0 = make_leaf(0, 2, 10, 10);
    BNode child1 = make_leaf(1000, 2, 10, 10);
    uint64_t ptr0 = pages.new_page(child0);
    uint64_t ptr1 = pages.new_page(child1);

    BNode parent(BTREE_PAGE_SIZE);
    parent.set_header(BNODE_NODE, 2);
    parent.node_append_kv(0, ptr0, child0.get_key(0), {});
    parent.node_append_kv(1, ptr1, child1.get_key(0), {});

    BTree tree{0, &pages};
    BNode result = node_delete(tree, parent, 0, child0.get_key(1));

    ASSERT_FALSE(result.data.empty());
    ASSERT_EQ(result.nkeys(), 1); // the 2 children merged into 1
    EXPECT_FALSE(pages.has_page(ptr0));
    EXPECT_FALSE(pages.has_page(ptr1));

    BNode merged = pages.get(result.get_ptr(0));
    ASSERT_EQ(merged.nkeys(), 3);
    EXPECT_EQ(merged.get_key(0), child0.get_key(0));
    EXPECT_EQ(merged.get_key(1), child1.get_key(0));
    EXPECT_EQ(merged.get_key(2), child1.get_key(1));
}

TEST(NodeDelete, WhenKeyNotFoundInChildSubtreeThenEmptyNodeIsReturned) {
    InMemoryPageManager pages;
    BNode child0 = make_leaf(0, 3, 20, 20);
    BNode child1 = make_leaf(1000, 3, 20, 20);
    uint64_t ptr0 = pages.new_page(child0);
    uint64_t ptr1 = pages.new_page(child1);

    BNode parent(BTREE_PAGE_SIZE);
    parent.set_header(BNODE_NODE, 2);
    parent.node_append_kv(0, ptr0, child0.get_key(0), {});
    parent.node_append_kv(1, ptr1, child1.get_key(0), {});

    BTree tree{0, &pages};
    BNode result = node_delete(tree, parent, 0, indexed_key(999, 20));

    EXPECT_TRUE(result.data.empty());
    EXPECT_TRUE(pages.has_page(ptr0)); // untouched since the key wasn't found
}

// ============================================================================
// BTree::insert: high-level KV interface
// ============================================================================
TEST(BTreeInsert, WhenTreeIsEmptyThenFirstInsertCreatesSentinelRoot) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};

    tree.insert(bytes("k1"), bytes("v1"));

    ASSERT_NE(tree.root, 0u);
    BNode root = pages.get(tree.root);
    EXPECT_EQ(root.btype(), BNODE_LEAF);
    EXPECT_EQ(root.nkeys(), 2); // sentinel + the inserted key
    EXPECT_TRUE(root.get_key(0).empty());
    EXPECT_EQ(str(root.get_key(1)), "k1");
    EXPECT_EQ(str(root.get_val(1)), "v1");
}

TEST(BTreeInsert, WhenKeyIsInsertedThenItIsRetrievable) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};

    tree.insert(bytes("k1"), bytes("v1"));

    auto val = tree.get(bytes("k1"));
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(str(*val), "v1");
    EXPECT_FALSE(tree.get(bytes("missing")).has_value());
}

TEST(BTreeInsert, WhenMultipleKeysAreInsertedThenAllAreRetrievable) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};

    tree.insert(bytes("k3"), bytes("v3"));
    tree.insert(bytes("k1"), bytes("v1"));
    tree.insert(bytes("k2"), bytes("v2"));

    EXPECT_EQ(str(*tree.get(bytes("k1"))), "v1");
    EXPECT_EQ(str(*tree.get(bytes("k2"))), "v2");
    EXPECT_EQ(str(*tree.get(bytes("k3"))), "v3");
}

TEST(BTreeInsert, WhenExistingKeyIsInsertedThenValueIsUpdated) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};

    tree.insert(bytes("k1"), bytes("v1"));
    tree.insert(bytes("k1"), bytes("updated"));

    EXPECT_EQ(str(*tree.get(bytes("k1"))), "updated");
}

TEST(BTreeInsert, WhenEnoughKeysAreInsertedThenRootSplitsAndTreeGrowsALevel) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};

    // Large values push a single page's worth of keys past BTREE_PAGE_SIZE,
    // forcing the root to split and the tree to grow to height 2.
    const int n = 30;
    for (int i = 0; i < n; ++i) {
        tree.insert(indexed_key(static_cast<uint32_t>(i), 100), std::vector<uint8_t>(100, 'v'));
    }

    BNode root = pages.get(tree.root);
    EXPECT_EQ(root.btype(), BNODE_NODE);

    for (int i = 0; i < n; ++i) {
        auto val = tree.get(indexed_key(static_cast<uint32_t>(i), 100));
        ASSERT_TRUE(val.has_value()) << "missing key " << i;
        EXPECT_EQ(val->size(), 100u);
    }
}

TEST(BTreeInsert, WhenKeyIsEmptyThenInsertThrows) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};
    EXPECT_THROW(tree.insert({}, bytes("v1")), std::invalid_argument);
}

TEST(BTreeInsert, WhenKeyExceedsMaxSizeThenInsertThrows) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};
    std::vector<uint8_t> big_key(BTREE_MAX_KEY_SIZE + 1, 'x');
    EXPECT_THROW(tree.insert(big_key, bytes("v1")), std::invalid_argument);
}

TEST(BTreeInsert, WhenValExceedsMaxSizeThenInsertThrows) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};
    std::vector<uint8_t> big_val(BTREE_MAX_VAL_SIZE + 1, 'x');
    EXPECT_THROW(tree.insert(bytes("k1"), big_val), std::invalid_argument);
}

// ============================================================================
// BTree::get: high-level KV interface
// ============================================================================
TEST(BTreeGet, WhenTreeIsEmptyThenReturnsNullopt) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};

    EXPECT_FALSE(tree.get(bytes("k1")).has_value());
}

TEST(BTreeGet, WhenKeyExistsThenReturnsItsValue) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};
    tree.insert(bytes("k1"), bytes("v1"));

    auto val = tree.get(bytes("k1"));
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(str(*val), "v1");
}

TEST(BTreeGet, WhenKeyDoesNotExistThenReturnsNullopt) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};
    tree.insert(bytes("k1"), bytes("v1"));

    EXPECT_FALSE(tree.get(bytes("missing")).has_value());
}

TEST(BTreeGet, WhenMultipleKeysExistThenEachIsRetrievable) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};
    tree.insert(bytes("k3"), bytes("v3"));
    tree.insert(bytes("k1"), bytes("v1"));
    tree.insert(bytes("k2"), bytes("v2"));

    EXPECT_EQ(str(*tree.get(bytes("k1"))), "v1");
    EXPECT_EQ(str(*tree.get(bytes("k2"))), "v2");
    EXPECT_EQ(str(*tree.get(bytes("k3"))), "v3");
}

TEST(BTreeGet, WhenKeyIsUpdatedThenGetReturnsLatestValue) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};
    tree.insert(bytes("k1"), bytes("v1"));
    tree.insert(bytes("k1"), bytes("updated"));

    EXPECT_EQ(str(*tree.get(bytes("k1"))), "updated");
}

TEST(BTreeGet, WhenKeyIsRemovedThenGetReturnsNullopt) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};
    tree.insert(bytes("k1"), bytes("v1"));
    ASSERT_TRUE(tree.remove(bytes("k1")));

    EXPECT_FALSE(tree.get(bytes("k1")).has_value());
}

TEST(BTreeGet, WhenTraversingMultiLevelTreeThenEveryKeyIsFound) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};

    // Mirrors BTreeInsert.WhenEnoughKeysAreInsertedThenRootSplitsAndTreeGrowsALevel:
    // enough large values to force the root to split into height 2.
    const int n = 30;
    for (int i = 0; i < n; ++i) {
        tree.insert(indexed_key(static_cast<uint32_t>(i), 100), std::vector<uint8_t>(100, 'v'));
    }
    ASSERT_EQ(pages.get(tree.root).btype(), BNODE_NODE);

    for (int i = 0; i < n; ++i) {
        auto val = tree.get(indexed_key(static_cast<uint32_t>(i), 100));
        ASSERT_TRUE(val.has_value()) << "missing key " << i;
        EXPECT_EQ(val->size(), 100u);
    }
    EXPECT_FALSE(tree.get(indexed_key(static_cast<uint32_t>(n), 100)).has_value());
}

TEST(BTreeGet, WhenKeyIsEmptyThenGetThrows) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};
    EXPECT_THROW(tree.get({}), std::invalid_argument);
}

TEST(BTreeGet, WhenKeyExceedsMaxSizeThenGetThrows) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};
    std::vector<uint8_t> big_key(BTREE_MAX_KEY_SIZE + 1, 'x');
    EXPECT_THROW(tree.get(big_key), std::invalid_argument);
}

// ============================================================================
// BTree::remove: high-level KV interface
// ============================================================================
TEST(BTreeRemove, WhenTreeIsEmptyThenRemoveReturnsFalse) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};

    EXPECT_FALSE(tree.remove(bytes("k1")));
}

TEST(BTreeRemove, WhenKeyIsRemovedThenItIsNoLongerRetrievable) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};
    tree.insert(bytes("k1"), bytes("v1"));

    EXPECT_TRUE(tree.remove(bytes("k1")));
    EXPECT_FALSE(tree.get(bytes("k1")).has_value());
}

TEST(BTreeRemove, WhenKeyDoesNotExistThenRemoveReturnsFalse) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};
    tree.insert(bytes("k1"), bytes("v1"));

    EXPECT_FALSE(tree.remove(bytes("missing")));
    EXPECT_TRUE(tree.get(bytes("k1")).has_value());
}

TEST(BTreeRemove, WhenOneOfMultipleKeysIsRemovedThenOthersRemainRetrievable) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};
    tree.insert(bytes("k1"), bytes("v1"));
    tree.insert(bytes("k2"), bytes("v2"));
    tree.insert(bytes("k3"), bytes("v3"));

    EXPECT_TRUE(tree.remove(bytes("k2")));

    EXPECT_EQ(str(*tree.get(bytes("k1"))), "v1");
    EXPECT_FALSE(tree.get(bytes("k2")).has_value());
    EXPECT_EQ(str(*tree.get(bytes("k3"))), "v3");
}

TEST(BTreeRemove, WhenRemovingAllKeysThenOnlyTheSentinelRemains) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};
    tree.insert(bytes("k1"), bytes("v1"));
    tree.insert(bytes("k2"), bytes("v2"));

    EXPECT_TRUE(tree.remove(bytes("k1")));
    EXPECT_TRUE(tree.remove(bytes("k2")));

    EXPECT_FALSE(tree.get(bytes("k1")).has_value());
    EXPECT_FALSE(tree.get(bytes("k2")).has_value());

    BNode root = pages.get(tree.root);
    EXPECT_EQ(root.btype(), BNODE_LEAF);
    EXPECT_EQ(root.nkeys(), 1); // only the empty sentinel key remains
}

TEST(BTreeRemove, WhenRemovingKeysAfterRootSplitThenTreeHeightCanShrinkBack) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};

    // Mirrors BTreeInsert.WhenEnoughKeysAreInsertedThenRootSplitsAndTreeGrowsALevel:
    // enough large values to force the root to split into height 2.
    const int n = 30;
    for (int i = 0; i < n; ++i) {
        tree.insert(indexed_key(static_cast<uint32_t>(i), 100), std::vector<uint8_t>(100, 'v'));
    }
    ASSERT_EQ(pages.get(tree.root).btype(), BNODE_NODE);

    for (int i = 0; i < n; ++i) {
        ASSERT_TRUE(tree.remove(indexed_key(static_cast<uint32_t>(i), 100))) << "missing key " << i;
    }
    for (int i = 0; i < n; ++i) {
        EXPECT_FALSE(tree.get(indexed_key(static_cast<uint32_t>(i), 100)).has_value());
    }

    BNode root = pages.get(tree.root);
    EXPECT_EQ(root.btype(), BNODE_LEAF); // shrunk back down from height 2
    EXPECT_EQ(root.nkeys(), 1); // only the sentinel remains
}

TEST(BTreeRemove, WhenKeyIsEmptyThenRemoveThrows) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};
    EXPECT_THROW(tree.remove({}), std::invalid_argument);
}

TEST(BTreeRemove, WhenKeyExceedsMaxSizeThenRemoveThrows) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};
    std::vector<uint8_t> big_key(BTREE_MAX_KEY_SIZE + 1, 'x');
    EXPECT_THROW(tree.remove(big_key), std::invalid_argument);
}

// ============================================================================
// Reference-model verification
//
// Wraps a BTree backed by an in-memory PageManager plus a reference
// std::map, and verifies the tree's structural invariants (node sizes
// within limits, keys sorted, internal keys mirroring their child's first
// key) alongside the data matching the reference.
// ============================================================================
namespace {
    struct BTreeReferenceModel {
        InMemoryPageManager pages;
        BTree tree{0, &pages};
        std::map<std::vector<uint8_t>, std::vector<uint8_t>> ref;

        void add(const std::vector<uint8_t>& key, const std::vector<uint8_t>& val) {
            tree.insert(key, val);
            ref[key] = val;
        }

        bool del(const std::vector<uint8_t>& key) {
            bool removed = tree.remove(key);
            if (removed) {
                ref.erase(key);
            }
            return removed;
        }

        // Checks structural invariants and that the tree's data matches
        // `ref` (plus the leading empty-key sentinel every root carries).
        void verify() const {
            ASSERT_NE(tree.root, 0u);
            std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> collected;
            verify_node(pages.get(tree.root), collected);

            for (size_t i = 0; i + 1 < collected.size(); ++i) {
                ASSERT_LT(collected[i].first, collected[i + 1].first)
                    << "flattened leaf keys must be globally sorted";
            }

            ASSERT_EQ(collected.size(), ref.size() + 1)
                << "leaf entries (minus the sentinel) must match the reference size";
            ASSERT_TRUE(collected[0].first.empty()) << "leftmost key must be the sentinel";

            auto ref_it = ref.begin();
            for (size_t i = 1; i < collected.size(); ++i, ++ref_it) {
                EXPECT_EQ(collected[i].first, ref_it->first);
                EXPECT_EQ(collected[i].second, ref_it->second);

                auto val = tree.get(ref_it->first);
                ASSERT_TRUE(val.has_value()) << "get() should find every key in ref";
                EXPECT_EQ(*val, ref_it->second);
            }
        }

    private:
        void verify_node(
            const BNode& node,
            std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>& out
        ) const {
            ASSERT_LE(node.nbytes(), BTREE_PAGE_SIZE);
            ASSERT_GE(node.nkeys(), 1);
            for (uint16_t i = 0; static_cast<uint16_t>(i + 1) < node.nkeys(); ++i) {
                ASSERT_LT(node.get_key(i), node.get_key(static_cast<uint16_t>(i + 1)))
                    << "keys must be sorted within a node";
            }

            if (node.btype() == BNODE_LEAF) {
                for (uint16_t i = 0; i < node.nkeys(); ++i) {
                    out.emplace_back(node.get_key(i), node.get_val(i));
                }
                return;
            }

            for (uint16_t i = 0; i < node.nkeys(); ++i) {
                BNode child = pages.get(node.get_ptr(i));
                ASSERT_EQ(node.get_key(i), child.get_key(0))
                    << "internal node key must mirror its child's first key";
                verify_node(child, out);
            }
        }
    };
}

TEST(BTreeReferenceModelTest, WhenManyKeysAreInsertedThenTreeStaysValidThroughout) {
    BTreeReferenceModel model;

    const int n = 300;
    for (int i = 0; i < n; ++i) {
        model.add(indexed_key(static_cast<uint32_t>(i), 20), std::vector<uint8_t>(20, 'v'));
        if (i % 10 == 0) {
            model.verify();
        }
    }
    model.verify();
}

TEST(BTreeReferenceModelTest, WhenManyKeysAreInsertedAndDeletedInRandomOrderThenTreeStaysValidThroughout) {
    BTreeReferenceModel model;
    std::mt19937 rng(12345);

    const int n = 300;
    std::vector<std::vector<uint8_t>> keys;
    for (int i = 0; i < n; ++i) {
        keys.push_back(indexed_key(static_cast<uint32_t>(i), 20));
    }
    std::shuffle(keys.begin(), keys.end(), rng);

    for (size_t i = 0; i < keys.size(); ++i) {
        model.add(keys[i], std::vector<uint8_t>(20, static_cast<uint8_t>('a' + (i % 26))));
        if (i % 10 == 0) {
            model.verify();
        }
    }
    model.verify();

    std::shuffle(keys.begin(), keys.end(), rng);
    for (size_t i = 0; i < keys.size(); ++i) {
        ASSERT_TRUE(model.del(keys[i])) << "key should have been present";
        if (i % 10 == 0) {
            model.verify();
        }
    }
    model.verify(); // only the sentinel should remain
}

TEST(BTreeReferenceModelTest, WhenInsertsAndDeletesAreInterleavedThenTreeStaysValidThroughout) {
    BTreeReferenceModel model;
    std::mt19937 rng(9001);
    std::uniform_int_distribution<int> op_dist(0, 2); // 2/3 chance of an insert vs. a delete
    std::uniform_int_distribution<uint32_t> key_dist(0, 99);

    for (int i = 0; i < 500; ++i) {
        uint32_t k = key_dist(rng);
        auto key = indexed_key(k, 20);
        if (op_dist(rng) != 0 || model.ref.find(key) == model.ref.end()) {
            model.add(key, std::vector<uint8_t>(20, static_cast<uint8_t>('a' + (k % 26))));
        } else {
            ASSERT_TRUE(model.del(key));
        }
        if (i % 10 == 0) {
            model.verify();
        }
    }
    model.verify();
}
