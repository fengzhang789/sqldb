#include "pagemanager.h"

#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
#include <algorithm>
#include <stdexcept>

namespace {
    constexpr size_t MIN_MMAP_SIZE = 64uz << 20; // 64 MiB growth floor
}

PageManager::PageManager(int fd, uint64_t flushed_pages) : fd_(fd), page_flushed_(flushed_pages) {
    // Map already-durable pages so get() can see them right away.
    if (page_flushed_ > 1) {
        extend_mmap(page_flushed_ * BTREE_PAGE_SIZE);
    }
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

BNode PageManager::get(uint64_t ptr) const {
    uint64_t start = 0;
    for (const auto& chunk : mmap_chunks_) {
        uint64_t end = start + chunk.size / BTREE_PAGE_SIZE;
        if (ptr < end) {
            size_t offset = BTREE_PAGE_SIZE * (ptr - start);
            std::vector<uint8_t> page(chunk.data + offset, chunk.data + offset + BTREE_PAGE_SIZE);
            return decode(page);
        }
        start = end;
    }
    throw std::out_of_range("PageManager: bad ptr");
}

uint64_t PageManager::new_page(const BNode& node) {
    uint64_t ptr = page_flushed_ + page_temp_.size();
    page_temp_.push_back(encode(node));
    return ptr;
}

void PageManager::del(uint64_t) {}

void PageManager::write_pages() {
    if (page_temp_.empty()) {
        return;
    }

    size_t size = (page_flushed_ + page_temp_.size()) * BTREE_PAGE_SIZE;
    extend_mmap(size);

    std::vector<iovec> iovs;
    iovs.reserve(page_temp_.size());
    for (auto& page : page_temp_) {
        iovs.push_back(iovec{page.data(), page.size()});
    }

    off_t offset = static_cast<off_t>(page_flushed_ * BTREE_PAGE_SIZE);
    ssize_t want = static_cast<ssize_t>(page_temp_.size() * BTREE_PAGE_SIZE);
    if (::pwritev(fd_, iovs.data(), static_cast<int>(iovs.size()), offset) != want) {
        throw std::runtime_error("PageManager: pwritev failed");
    }

    page_flushed_ += page_temp_.size();
    page_temp_.clear();
}

