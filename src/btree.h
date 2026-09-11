#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>
#include <cassert>

// Constants
constexpr uint16_t BNODE_NODE = 1; // Internal node with child page pointers
constexpr uint16_t BNODE_LEAF = 2; // Leaf node with values
constexpr size_t BTREE_PAGE_SIZE = 4096;

// Key-Value Pair Representation
struct KVPair {
    std::vector<uint8_t> key;
    std::vector<uint8_t> val;
};

// In-Memory B+Tree Node
struct Node {
    uint16_t type;                // BNODE_NODE or BNODE_LEAF
    std::vector<uint64_t> children;   // Child Page IDs (for internal nodes)
    std::vector<KVPair> pairs; // Keys and Values
};

// Function Declarations
std::vector<uint8_t> encode(const Node& node);
Node decode(const std::vector<uint8_t>& page);

// Layout Helper Function Declarations
uint16_t btype(const uint8_t* page);
uint16_t nkeys(const uint8_t* page);
size_t ptrsOffset();
size_t offsetsOffset(const uint8_t* page);
size_t kvBaseOffset(const uint8_t* page);
uint64_t getPtr(const uint8_t* page, uint16_t idx);
uint16_t getOffset(const uint8_t* page, uint16_t idx);
size_t kvPos(const uint8_t* page, uint16_t idx);
std::vector<uint8_t> getKey(const uint8_t* page, uint16_t idx);
std::vector<uint8_t> getVal(const uint8_t* page, uint16_t idx);

