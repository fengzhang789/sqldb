#include <iostream>
#include "btree.h"

int main() {
    // Construct a leaf node with test key-values: {"k1":"hi", "k3":"hello"}
    BNode original_leaf(BTREE_PAGE_SIZE);
    original_leaf.set_header(BNODE_LEAF, 2);
    original_leaf.node_append_kv(0, 0, {'k', '1'}, {'h', 'i'});
    original_leaf.node_append_kv(1, 0, {'k', '3'}, {'h', 'e', 'l', 'l', 'o'});

    // Encode to 4KB page
    std::vector<uint8_t> serialized_page = encode(original_leaf);

    // Decode from 4KB page
    BNode decoded_leaf = decode(serialized_page);

    // Print Results
    std::cout << "Node Type: " << decoded_leaf.btype() << " (Expected 2)\n";
    std::cout << "Number of Keys: " << decoded_leaf.nkeys() << " (Expected 2)\n";

    for (uint16_t i = 0; i < decoded_leaf.nkeys(); ++i) {
        std::vector<uint8_t> key = decoded_leaf.get_key(i);
        std::vector<uint8_t> val = decoded_leaf.get_val(i);
        std::string key_str(key.begin(), key.end());
        std::string val_str(val.begin(), val.end());
        std::cout << "  Key: " << key_str << " | Value: " << val_str << "\n";
    }

    return 0;
}
