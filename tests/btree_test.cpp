#include "btree.h"

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

