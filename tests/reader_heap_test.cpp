#include "storage/reader_heap.h"

#include <cstdint>
#include <memory>
#include <random>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include "storage/kv_reader.h"

namespace {
    // Pushes a new reader at each version, keeping the readers alive in *pool.
    void push_versions(ReaderHeap& heap, std::vector<std::unique_ptr<KVReader>>* pool,
                       const std::vector<uint64_t>& versions) {
        for (uint64_t version : versions) {
            pool->push_back(std::make_unique<KVReader>());
            pool->back()->version = version;
            heap.push(pool->back().get());
        }
    }

    KVReader* find_version(const std::vector<std::unique_ptr<KVReader>>& pool, uint64_t version) {
        for (const auto& reader : pool) {
            if (reader->version == version) {
                return reader.get();
            }
        }
        return nullptr;
    }

    // Removes the oldest reader until the heap is empty, returning the versions in removal order.
    std::vector<uint64_t> drain(ReaderHeap& heap) {
        std::vector<uint64_t> versions;
        while (!heap.empty()) {
            versions.push_back(heap.remove(heap.min()->heap_index)->version);
        }
        return versions;
    }

    // Every live reader's heap_index names its own slot, and no reader is older than the reader in its parent slot.
    void expect_heap_ordered(const ReaderHeap& heap, const std::vector<KVReader*>& live) {
        ASSERT_EQ(heap.size(), live.size());
        std::vector<const KVReader*> slots(live.size(), nullptr);
        for (const KVReader* reader : live) {
            ASSERT_GE(reader->heap_index, 0);
            ASSERT_LT(static_cast<size_t>(reader->heap_index), slots.size());
            ASSERT_EQ(slots[reader->heap_index], nullptr) << "2 readers claim slot " << reader->heap_index;
            slots[reader->heap_index] = reader;
        }
        for (size_t i = 1; i < slots.size(); ++i) {
            ASSERT_LE(slots[(i - 1) / 2]->version, slots[i]->version) << "slot " << i;
        }
        ASSERT_EQ(heap.min(), slots.empty() ? nullptr : slots[0]);
    }
}

// ============================================================================
// ReaderHeap: push
// ============================================================================
TEST(ReaderHeapTest, WhenTheHeapIsEmptyThenMinIsNull) {
    ReaderHeap heap;

    EXPECT_TRUE(heap.empty());
    EXPECT_EQ(heap.size(), 0u);
    EXPECT_EQ(heap.min(), nullptr);
}

TEST(ReaderHeapTest, WhenReadersArePushedThenMinIsTheOldestVersionSoFar) {
    ReaderHeap heap;
    std::vector<std::unique_ptr<KVReader>> pool;

    const std::vector<uint64_t> versions = {5, 3, 8, 1, 4};
    const std::vector<uint64_t> oldest = {5, 3, 3, 1, 1};
    for (size_t i = 0; i < versions.size(); ++i) {
        push_versions(heap, &pool, {versions[i]});
        EXPECT_EQ(heap.min()->version, oldest[i]) << "after pushing " << versions[i];
    }
    EXPECT_EQ(heap.size(), versions.size());
}

TEST(ReaderHeapTest, WhenReadersArePushedThenEachHeapIndexNamesItsOwnSlot) {
    ReaderHeap heap;
    std::vector<std::unique_ptr<KVReader>> pool;
    push_versions(heap, &pool, {9, 7, 5, 3, 1, 8, 6, 4, 2});

    std::vector<KVReader*> live;
    for (const auto& reader : pool) {
        live.push_back(reader.get());
    }
    expect_heap_ordered(heap, live);
}

// ============================================================================
// ReaderHeap: remove
// ============================================================================
TEST(ReaderHeapTest, WhenTheMinimumIsRemovedThenTheNextOldestBecomesMin) {
    ReaderHeap heap;
    std::vector<std::unique_ptr<KVReader>> pool;
    push_versions(heap, &pool, {5, 3, 8, 1, 4});

    KVReader* oldest = heap.min();
    EXPECT_EQ(heap.remove(oldest->heap_index), oldest);

    EXPECT_EQ(oldest->heap_index, -1);
    EXPECT_EQ(heap.size(), 4u);
    EXPECT_EQ(heap.min()->version, 3u);
}

TEST(ReaderHeapTest, WhenANonMinimumReaderIsRemovedThenTheRestStillComeOutInVersionOrder) {
    ReaderHeap heap;
    std::vector<std::unique_ptr<KVReader>> pool;
    push_versions(heap, &pool, {5, 3, 8, 1, 4});

    KVReader* reader = find_version(pool, 5);
    EXPECT_EQ(heap.remove(reader->heap_index), reader);

    EXPECT_EQ(reader->heap_index, -1);
    EXPECT_EQ(heap.min()->version, 1u);
    EXPECT_EQ(drain(heap), (std::vector<uint64_t>{1, 3, 4, 8}));
}

// Removing a slot moves the last reader into it, and when that reader is older than the slot's parent, sifting down
// alone would leave it out of order.
TEST(ReaderHeapTest, WhenTheReaderMovedIntoARemovedSlotIsOlderThanItsParentThenItMovesUp) {
    ReaderHeap heap;
    std::vector<std::unique_ptr<KVReader>> pool;
    push_versions(heap, &pool, {1, 10, 2, 11, 12, 5}); // slots: 11 sits under 10, and 5 is the last slot, under 2

    KVReader* reader = find_version(pool, 11);
    heap.remove(reader->heap_index);

    std::vector<KVReader*> live;
    for (const auto& r : pool) {
        if (r.get() != reader) {
            live.push_back(r.get());
        }
    }
    expect_heap_ordered(heap, live);
    EXPECT_EQ(drain(heap), (std::vector<uint64_t>{1, 2, 5, 10, 12}));
}

TEST(ReaderHeapTest, WhenReadersShareAVersionThenRemovingOneLeavesTheOther) {
    ReaderHeap heap;
    KVReader a;
    KVReader b;
    a.version = 4;
    b.version = 4;
    heap.push(&a);
    heap.push(&b);

    EXPECT_EQ(heap.remove(a.heap_index), &a);

    EXPECT_EQ(heap.min(), &b);
    EXPECT_EQ(b.heap_index, 0);
}

TEST(ReaderHeapTest, WhenTheLastReaderIsRemovedThenTheHeapIsEmptyAndCanBeReused) {
    ReaderHeap heap;
    KVReader reader;
    reader.version = 7;

    heap.push(&reader);
    EXPECT_EQ(reader.heap_index, 0);
    EXPECT_EQ(heap.remove(0), &reader);

    EXPECT_TRUE(heap.empty());
    EXPECT_EQ(heap.min(), nullptr);
    EXPECT_EQ(reader.heap_index, -1);

    heap.push(&reader);
    EXPECT_EQ(heap.min(), &reader);
}

TEST(ReaderHeapTest, WhenAnIndexOutsideTheHeapIsRemovedThenItAsserts) {
    ReaderHeap heap;
    EXPECT_DEATH(heap.remove(0), "");

    KVReader reader;
    heap.push(&reader);
    EXPECT_DEATH(heap.remove(1), "");
    EXPECT_DEATH(heap.remove(-1), "");
}

// Readers begin and end in any order, like transactions on different threads.
TEST(ReaderHeapTest, WhenReadersComeAndGoInRandomOrderThenMinAlwaysMatchesAReferenceMultiset) {
    ReaderHeap heap;
    std::vector<std::unique_ptr<KVReader>> pool;
    std::vector<KVReader*> live;
    std::multiset<uint64_t> model;
    std::mt19937 rng(20260915);

    for (int op = 0; op < 2000; ++op) {
        if (live.empty() || rng() % 2 == 0) {
            push_versions(heap, &pool, {rng() % 50}); // few versions, so many readers share one
            live.push_back(pool.back().get());
            model.insert(pool.back()->version);
        } else {
            size_t victim = rng() % live.size();
            KVReader* reader = live[victim];
            ASSERT_EQ(heap.remove(reader->heap_index), reader) << "op " << op;
            model.erase(model.find(reader->version));
            live[victim] = live.back();
            live.pop_back();
        }

        ASSERT_EQ(heap.size(), model.size()) << "op " << op;
        if (!model.empty()) {
            ASSERT_EQ(heap.min()->version, *model.begin()) << "op " << op;
        }
        ASSERT_NO_FATAL_FAILURE(expect_heap_ordered(heap, live)) << "op " << op;
    }
}
