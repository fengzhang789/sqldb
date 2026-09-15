#pragma once

#include <cstddef>
#include <vector>

struct KVReader; // see kv_reader.h

// ReaderHeap is a min-heap of the open readers ordered by version, so a writer can find the oldest version still being
// read. Like Go's container/heap, each reader records its slot in heap_index, so it can be removed from anywhere in
// O(log n) rather than only popped as the minimum.
class ReaderHeap {
    public:
        void push(KVReader* reader); // sets reader->heap_index
        KVReader* remove(int index); // removes and returns the reader at index, resetting its heap_index to -1
        KVReader* min() const; // the oldest reader, or nullptr if empty

        size_t size() const { return items_.size(); }
        bool empty() const { return items_.empty(); }

    private:
        bool less(size_t i, size_t j) const;
        void swap(size_t i, size_t j);
        void up(size_t j);
        bool down(size_t i0); // true if the item moved

        std::vector<KVReader*> items_;
};
