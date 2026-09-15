#include "storage/reader_heap.h"

#include <cassert>
#include <utility>

#include "storage/kv_reader.h"

void ReaderHeap::push(KVReader* reader) {
    reader->heap_index = static_cast<int>(items_.size());
    items_.push_back(reader);
    up(items_.size() - 1);
}

// The last reader fills the removed slot, then sifts down or up, whichever its new position needs.
KVReader* ReaderHeap::remove(int index) {
    assert(index >= 0 && static_cast<size_t>(index) < items_.size());
    size_t i = static_cast<size_t>(index);
    swap(i, items_.size() - 1);
    KVReader* reader = items_.back();
    items_.pop_back();
    if (i < items_.size() && !down(i)) {
        up(i);
    }
    reader->heap_index = -1;
    return reader;
}

KVReader* ReaderHeap::min() const {
    return items_.empty() ? nullptr : items_.front();
}

bool ReaderHeap::less(size_t i, size_t j) const {
    return items_[i]->version < items_[j]->version;
}

void ReaderHeap::swap(size_t i, size_t j) {
    std::swap(items_[i], items_[j]);
    items_[i]->heap_index = static_cast<int>(i);
    items_[j]->heap_index = static_cast<int>(j);
}

void ReaderHeap::up(size_t j) {
    while (j > 0) {
        size_t parent = (j - 1) / 2;
        if (!less(j, parent)) {
            break;
        }
        swap(parent, j);
        j = parent;
    }
}

bool ReaderHeap::down(size_t i0) {
    size_t i = i0;
    while (2 * i + 1 < items_.size()) {
        size_t kid = 2 * i + 1;
        if (kid + 1 < items_.size() && less(kid + 1, kid)) {
            ++kid; // the older of the 2 children
        }
        if (!less(kid, i)) {
            break;
        }
        swap(i, kid);
        i = kid;
    }
    return i > i0;
}
