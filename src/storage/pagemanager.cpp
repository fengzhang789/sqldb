#include "storage/pagemanager.h"

#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
#include <algorithm>
#include <stdexcept>

namespace {
    constexpr size_t MIN_MMAP_SIZE = 64uz << 20; // 64 MiB growth floor
    constexpr size_t MAX_IOVS = 64; // stay well within IOV_MAX per pwritev()
}

PageManager::PageManager(int fd)
    : fd_(fd),
      page_flushed_(2),
      page_updates_{{1, std::vector<uint8_t>(BTREE_PAGE_SIZE, 0)}}, // the empty list node
      free_(this, FreeListState{.head_page = 1, .head_seq = 0, .tail_page = 1, .tail_seq = 0}) {}

PageManager::PageManager(int fd, uint64_t flushed_pages, const FreeListState& free_state)
    : fd_(fd), page_flushed_(flushed_pages), free_(this, free_state) {
    // Map already-durable pages so get() can see them right away.
    extend_mmap(page_flushed_ * BTREE_PAGE_SIZE);
}

PageManager::~PageManager() {
    for (const auto& chunk : mmap_chunks_) {
        ::munmap(chunk.data, chunk.size);
    }
}

// Grows the mmap'd range (never shrinks/moves it) so it covers at least
// `size` bytes, doubling each time so repeated growth doesn't accumulate
// too many separate mappings.
void PageManager::extend_mmap(size_t size) {
    if (size <= mmap_total_) {
        return;
    }

    size_t alloc = std::max(mmap_total_, MIN_MMAP_SIZE);
    while (mmap_total_ + alloc < size) {
        alloc *= 2;
    }

    void* addr = ::mmap(nullptr, alloc, PROT_READ, MAP_SHARED, fd_, static_cast<off_t>(mmap_total_));
    if (addr == MAP_FAILED) {
        throw std::runtime_error("PageManager: mmap failed");
    }

    mmap_chunks_.push_back({static_cast<uint8_t*>(addr), alloc});
    mmap_total_ += alloc;
}

const uint8_t* PageManager::mmap_page(uint64_t ptr) const {
    uint64_t start = 0;
    for (const auto& chunk : mmap_chunks_) {
        uint64_t end = start + chunk.size / BTREE_PAGE_SIZE;
        if (ptr < end) {
            return chunk.data + BTREE_PAGE_SIZE * (ptr - start);
        }
        start = end;
    }
    throw std::out_of_range("PageManager: bad ptr");
}

BNode PageManager::get(uint64_t ptr) const {
    const uint8_t* page = read_page(ptr);
    return decode(std::vector<uint8_t>(page, page + BTREE_PAGE_SIZE));
}

uint64_t PageManager::new_page(const BNode& node) {
    uint64_t ptr = free_.pop_head();
    if (ptr == 0) {
        ptr = page_flushed_ + page_append_;
        ++page_append_;
    }
    page_updates_[ptr] = encode(node);
    return ptr;
}

void PageManager::del(uint64_t ptr) {
    free_.push_tail(ptr);
}

// A buffered page shadows the durable one, since an update may read back a page
// it has already written.
const uint8_t* PageManager::read_page(uint64_t ptr) const {
    if (auto it = page_updates_.find(ptr); it != page_updates_.end()) {
        return it->second.data();
    }
    return mmap_page(ptr);
}

uint8_t* PageManager::write_page(uint64_t ptr) {
    auto it = page_updates_.find(ptr);
    if (it == page_updates_.end()) {
        const uint8_t* durable = mmap_page(ptr);
        it = page_updates_.emplace(ptr, std::vector<uint8_t>(durable, durable + BTREE_PAGE_SIZE)).first;
    }
    return it->second.data();
}

uint64_t PageManager::append_page() {
    uint64_t ptr = page_flushed_ + page_append_;
    ++page_append_;
    page_updates_[ptr] = std::vector<uint8_t>(BTREE_PAGE_SIZE, 0);
    return ptr;
}

void PageManager::set_versions(uint64_t version, uint64_t min_reader) {
    free_.version = version;
    free_.min_reader = min_reader;
}

void PageManager::revert(uint64_t flushed_pages, const FreeListState& free_state) {
    page_flushed_ = flushed_pages;
    page_append_ = 0;
    page_updates_.clear();
    free_.revert(free_state);
}

// Buffered pages mix appends with in-place rewrites of reused pages, so they're
// written by page number, coalescing consecutive runs into one pwritev().
void PageManager::write_pages() {
    if (page_updates_.empty()) {
        return;
    }
    extend_mmap((page_flushed_ + page_append_) * BTREE_PAGE_SIZE);

    auto it = page_updates_.begin();
    while (it != page_updates_.end()) {
        uint64_t first = it->first;
        std::vector<iovec> iovs;
        for (uint64_t ptr = first;
             it != page_updates_.end() && it->first == ptr && iovs.size() < MAX_IOVS;
             ++it, ++ptr) {
            iovs.push_back(iovec{it->second.data(), it->second.size()});
        }

        off_t offset = static_cast<off_t>(first * BTREE_PAGE_SIZE);
        ssize_t want = static_cast<ssize_t>(iovs.size() * BTREE_PAGE_SIZE);
        if (::pwritev(fd_, iovs.data(), static_cast<int>(iovs.size()), offset) != want) {
            throw std::runtime_error("PageManager: pwritev failed");
        }
    }

    page_flushed_ += page_append_;
    page_append_ = 0;
    page_updates_.clear();
}
