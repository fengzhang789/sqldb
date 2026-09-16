#include "storage/kv_reader.h"

#include <stdexcept>

std::optional<std::vector<uint8_t>> KVReader::get(const std::vector<uint8_t>& key) const {
    return tree_.get(key);
}

BIter KVReader::seek(const std::vector<uint8_t>& key, CMP cmp) const {
    return tree_.seek(key, cmp);
}

const uint8_t* KVReader::page_get_mapped(uint64_t ptr) const {
    uint64_t start = 0;
    for (const MmapChunk& chunk : chunks_) {
        uint64_t end = start + chunk.size / BTREE_PAGE_SIZE;
        if (ptr < end) {
            return chunk.data + BTREE_PAGE_SIZE * (ptr - start);
        }
        start = end;
    }
    throw std::out_of_range("KVReader: bad ptr");
}

BNode KVReader::MappedPages::get(uint64_t ptr) const {
    const uint8_t* page = reader->page_get_mapped(ptr);
    return decode(std::vector<uint8_t>(page, page + BTREE_PAGE_SIZE));
}

uint64_t KVReader::MappedPages::new_page(const BNode&) {
    throw std::logic_error("KVReader: a reader can't write");
}

void KVReader::MappedPages::del(uint64_t) {
    throw std::logic_error("KVReader: a reader can't write");
}
