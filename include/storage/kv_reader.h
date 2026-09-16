#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "storage/btree.h"
#include "storage/btree_iter.h"
#include "storage/pagemanager.h"

// One read-only mmap region of the file. KV only adds regions, and unmaps them at close.
struct MmapChunk {
    uint8_t* data;
    size_t size; // bytes
};

// KVReader is a read-only snapshot of one KV version: the tree root and mmap regions as of KV::begin_read. No page it
// can reach is reused while it is open (see FreeList), so it reads straight from the mmap without taking any lock,
// alongside other readers and the writer. Begin and end it through KV, on any thread.
struct KVReader {
    public:
        KVReader() = default;
        KVReader(const KVReader&) = delete;
        KVReader& operator=(const KVReader&) = delete;

        std::optional<std::vector<uint8_t>> get(const std::vector<uint8_t>& key) const;
        BIter seek(const std::vector<uint8_t>& key, CMP cmp) const; // usable until the reader ends
        const uint8_t* page_get_mapped(uint64_t ptr) const; // durable page bytes; throws if the snapshot's mmap lacks it

        uint64_t version = 0; // the commits this snapshot includes
        int heap_index = -1; // slot in KV's ReaderHeap; -1 while not in one

    protected:
        friend struct KV;

        BTree tree_; // a KVTX points its pages at its own view of the file instead
        std::vector<MmapChunk> chunks_; // a copy, since the writer may map more while this reader is open

    private:
        // Serves the tree's reads from the mmap; a reader never writes.
        struct MappedPages : IPageManager {
            explicit MappedPages(const KVReader* reader) : reader(reader) {}
            BNode get(uint64_t ptr) const override;
            uint64_t new_page(const BNode& node) override;
            void del(uint64_t ptr) override;
            const KVReader* reader;
        };

        MappedPages mapped_{this};
};
