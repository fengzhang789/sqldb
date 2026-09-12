#pragma once

#include <cstdint>
#include <vector>

#include "btree.h"

// IPageManager isolates the B+tree data structure from how pages are
// actually stored, so the tree can be tested with an in-memory
// implementation and later backed by a real file/mmap without changing any
// tree logic.
struct IPageManager {
    virtual ~IPageManager() = default;
    virtual BNode get(uint64_t ptr) const = 0;
    virtual uint64_t new_page(const BNode& node) = 0;
    virtual void del(uint64_t ptr) = 0;
};

// PageManager is an mmap-backed IPageManager. Existing pages are read
// straight out of a growable set of read-only mmap regions covering the
// file; newly created pages are buffered in memory (new_page) until
// write_pages() appends them to the file and folds them into the readable
// range. Ptr 0 is never handed out: it's reserved for the file's meta page
// (page 0), keeping it distinct from BTree's root == 0 "empty tree" sentinel.
class PageManager : public IPageManager {
    public:
        // `flushed_pages` is the on-disk page count restored from the meta
        // page when reopening a file; defaults to 1 for a fresh file.
        explicit PageManager(int fd, uint64_t flushed_pages = 1);
        ~PageManager() override;
        PageManager(const PageManager&) = delete;
        PageManager& operator=(const PageManager&) = delete;

        BNode get(uint64_t ptr) const override;
        uint64_t new_page(const BNode& node) override;
        void del(uint64_t ptr) override; // no-op: pages are reclaimed once a free list exists

        void write_pages(); // append pending new_page() pages to the file
        uint64_t flushed_pages() const { return page_flushed_; }

    private:
        struct MmapChunk {
            uint8_t* data;
            size_t size; // bytes
        };

        int fd_ = -1;
        size_t mmap_total_ = 0; // total mapped bytes, can exceed the file size
        std::vector<MmapChunk> mmap_chunks_;

        uint64_t page_flushed_ = 1; // pages already on disk; starts at 1 (page 0 is the meta page)
        std::vector<std::vector<uint8_t>> page_temp_; // pages from new_page() not yet flushed

        void extend_mmap(size_t size); // grow the mmap so it covers at least `size` bytes
};
