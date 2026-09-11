#include <iostream>
#include "btree.h"

int main() {
    // Construct a leaf node with test key-values
    Node original_leaf;
    original_leaf.type = BNODE_LEAF;
    
    KVPair kv1 = { {'k', '1'}, {'h', 'i'} };               // "k1": "hi"
    KVPair kv2 = { {'k', '3'}, {'h', 'e', 'l', 'l', 'o'} }; // "k3": "hello"
    
    original_leaf.pairs.push_back(kv1);
    original_leaf.pairs.push_back(kv2);

    // Encode to 4KB page
    std::vector<uint8_t> serialized_page = encode(original_leaf);

    // Decode from 4KB page
    Node decoded_leaf = decode(serialized_page);

    // Print Results
    std::cout << "Node Type: " << decoded_leaf.type << " (Expected 2)\n";
    std::cout << "Number of Keys: " << decoded_leaf.pairs.size() << " (Expected 2)\n";

    for (const auto& kv : decoded_leaf.pairs) {
        std::string key(kv.key.begin(), kv.key.end());
        std::string val(kv.val.begin(), kv.val.end());
        std::cout << "  Key: " << key << " | Value: " << val << "\n";
    }

    return 0;
}

