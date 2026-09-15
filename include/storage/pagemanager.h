#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "storage/btree.h"
#include "storage/freelist.h"

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

// PageManager is an mmap-backed IPageManager. Durable pages are read straight
// out of a growable set of read-only mmap regions covering the file, while
// pending writes are buffered in memory until write_pages() flushes them.
//
// Freed pages go to a FreeList and are reused by later updates, so a page is
// written more than once and the buffered pages are keyed by page number
// rather than forming an append queue. Reuse stays crash-safe because a page
// is only reusable once an update that no longer references it is durable
// (set_versions()).
//
// Ptr 0 is never handed out: it's reserved for the file's meta page (page 0),
// keeping it distinct from BTree's root == 0 "empty tree" sentinel. Page 1 is
// reserved for the free list's first node.
class PageManager : public IPageManager, private IFreeListPages {
    public:
        // A fresh (zero-length) file: the free list's empty first node is
        // buffered for the first write_pages() to lay down.
        explicit PageManager(int fd);

        // Reopens a file with the page count and free list state from its meta page.
        PageManager(int fd, uint64_t flushed_pages, const FreeListState& free_state);

        ~PageManager() override;
        PageManager(const PageManager&) = delete;
        PageManager& operator=(const PageManager&) = delete;

        BNode get(uint64_t ptr) const override;
        uint64_t new_page(const BNode& node) override; // reuse a free page, else append
        void del(uint64_t ptr) override; // hand the page to the free list

        void write_pages(); // write the pages buffered by this update
        void set_versions(uint64_t version, uint64_t min_reader); // for the free list, see FreeList::version

        uint64_t flushed_pages() const { return page_flushed_; }
        const FreeListState& free_state() const { return free_.state(); }

        // Reverts to a durable page count and free list state, discarding
        // buffered pages, e.g. after a failed update: nothing durable
        // references them, so their page numbers are safe to hand out again.
        void revert(uint64_t flushed_pages, const FreeListState& free_state);

    private:
        struct MmapChunk {
            uint8_t* data;
            size_t size; // bytes
        };

        // IFreeListPages: the free list keeps its nodes in the same file.
        const uint8_t* read_page(uint64_t ptr) const override;
        uint8_t* write_page(uint64_t ptr) override;
        uint64_t append_page() override;

        void extend_mmap(size_t size); // grow the mmap so it covers at least `size` bytes
        const uint8_t* mmap_page(uint64_t ptr) const; // durable page bytes, read-only

        int fd_ = -1;
        size_t mmap_total_ = 0; // total mapped bytes, can exceed the file size
        std::vector<MmapChunk> mmap_chunks_;

        uint64_t page_flushed_ = 2; // pages already on disk (page 0: meta, page 1: free list)
        uint64_t page_append_ = 0; // pages this update appends past page_flushed_
        std::map<uint64_t, std::vector<uint8_t>> page_updates_; // buffered writes, by page number

        FreeList free_;
};
