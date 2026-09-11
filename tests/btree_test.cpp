#include "btree.h"

#include <cstdio>
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

