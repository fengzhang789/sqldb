// Meta page (page 0) layout:
// +-----------+----------+---------------+-----------+----------+-----------+----------+---------+---------+
// | signature | root ptr | flushed pages | head page | head seq | tail page | tail seq | version | unused  |
// |   (16B)   |   (8B)   |     (8B)      |   (8B)    |   (8B)   |   (8B)    |   (8B)   |  (8B)   | (bytes) |
// +-----------+----------+---------------+-----------+----------+-----------+----------+---------+---------+
// The head/tail fields are the free list's position (see freelist.h), and version is the commit count its items are
// recorded with; committing them with the tree root is what makes page reuse crash-safe.
// Updated via a single pwrite() at offset 0, which is expected to be
// power-loss-atomic since it's page-aligned and touches a single sector.

#include "storage/kv.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace {
    constexpr char DB_SIG[] = "DB";
    constexpr size_t META_SIG_SIZE = 16;
    constexpr size_t MIN_MMAP_SIZE = 64uz << 20; // 64 MiB growth floor
    constexpr size_t MAX_IOVS = 64; // stay well within IOV_MAX per pwritev()

    // The 8-byte fields following the signature, in order.
    enum MetaField : size_t {
        META_ROOT,
        META_FLUSHED,
        META_HEAD_PAGE,
        META_HEAD_SEQ,
        META_TAIL_PAGE,
        META_TAIL_SEQ,
        META_VERSION,
        META_FIELD_COUNT,
    };
    constexpr size_t META_DATA_SIZE = META_SIG_SIZE + META_FIELD_COUNT * 8;

    static_assert(sizeof(DB_SIG) - 1 <= META_SIG_SIZE, "DB_SIG must fit within the signature field");
    static_assert(META_DATA_SIZE <= BTREE_PAGE_SIZE, "the meta page must fit in a single page");

    size_t meta_offset(MetaField field) {
        return META_SIG_SIZE + static_cast<size_t>(field) * 8;
    }

    void write_u64(uint8_t* ptr, uint64_t val) {
        std::memcpy(ptr, &val, sizeof(val));
    }

    uint64_t read_u64(const uint8_t* ptr) {
        uint64_t val;
        std::memcpy(&val, ptr, sizeof(val));
        return val;
    }

    // The decoded meta page: the state of the last committed update.
    struct Meta {
        uint64_t root = 0;
        uint64_t flushed = 0;
        FreeListState free_state;
        uint64_t version = 0;
    };

    Meta parse_meta(const std::vector<uint8_t>& data) {
        Meta meta;
        meta.root = read_u64(data.data() + meta_offset(META_ROOT));
        meta.flushed = read_u64(data.data() + meta_offset(META_FLUSHED));
        meta.free_state.head_page = read_u64(data.data() + meta_offset(META_HEAD_PAGE));
        meta.free_state.head_seq = read_u64(data.data() + meta_offset(META_HEAD_SEQ));
        meta.free_state.tail_page = read_u64(data.data() + meta_offset(META_TAIL_PAGE));
        meta.free_state.tail_seq = read_u64(data.data() + meta_offset(META_TAIL_SEQ));
        meta.version = read_u64(data.data() + meta_offset(META_VERSION));
        return meta;
    }
}

// KV: lifecycle
KV::KV(std::string path) : path(std::move(path)) {}

KV::~KV() {
    close();
}

// Creates (or opens) `path`, fsyncing the parent directory afterwards so the
// new directory entry is durable.
int KV::create_file_sync(const std::string& path) {
    std::filesystem::path p(path);
    std::filesystem::path dir = p.parent_path();
    if (dir.empty()) {
        dir = ".";
    }

    int dirfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) {
        throw std::runtime_error(std::string("KV: open directory failed: ") + std::strerror(errno));
    }

    int fd = ::openat(dirfd, p.filename().c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        int err = errno;
        ::close(dirfd);
        throw std::runtime_error(std::string("KV: open file failed: ") + std::strerror(err));
    }

    if (::fsync(dirfd) != 0) {
        int err = errno;
        ::close(fd); // may leave an empty file
        ::close(dirfd);
        throw std::runtime_error(std::string("KV: fsync directory failed: ") + std::strerror(err));
    }
    ::close(dirfd);
    return fd;
}

void KV::open() {
    fd_ = create_file_sync(path);
    read_root();
}

void KV::close() {
    if (fd_ < 0) {
        return;
    }
    assert(readers_.empty() && "KV::close: a reader is still open");
    ::fsync(fd_);
    ::close(fd_);
    fd_ = -1;
    for (const MmapChunk& chunk : chunks_) {
        ::munmap(chunk.data, chunk.size);
    }
    chunks_.clear();
    mmap_total_ = 0;
}

// Grows the mmap'd range (never shrinks/moves it) so it covers at least
// `size` bytes, doubling each time so repeated growth doesn't accumulate
// too many separate mappings.
void KV::extend_mmap(size_t size) {
    if (size <= mmap_total_) {
        return;
    }

    size_t alloc = std::max(mmap_total_, MIN_MMAP_SIZE);
    while (mmap_total_ + alloc < size) {
        alloc *= 2;
    }

    void* addr = ::mmap(nullptr, alloc, PROT_READ, MAP_SHARED, fd_, static_cast<off_t>(mmap_total_));
    if (addr == MAP_FAILED) {
        throw std::runtime_error("KV: mmap failed");
    }

    std::lock_guard lock(mu_); // readers copy chunks_
    chunks_.push_back({static_cast<uint8_t*>(addr), alloc});
    mmap_total_ += alloc;
}

// KVTX: writes on the tx's own copy-on-write tree
void KVTX::set(const std::vector<uint8_t>& key, const std::vector<uint8_t>& val) {
    tree_.insert(key, val); // throws (e.g. length limit) without mutating anything
}

bool KVTX::del(const std::vector<uint8_t>& key) {
    return tree_.remove(key);
}

bool KVTX::update(InsertReq* req) {
    std::optional<std::vector<uint8_t>> old = get(req->key);
    req->added = !old.has_value();
    req->old = std::move(old).value_or(std::vector<uint8_t>{});
    if ((req->mode == UpdateMode::INSERT_ONLY && !req->added) || (req->mode == UpdateMode::UPDATE_ONLY && req->added)) {
        return false;
    }
    set(req->key, req->val);
    return true;
}

bool KVTX::del(DeleteReq* req) {
    std::optional<std::vector<uint8_t>> old = get(req->key);
    if (!old.has_value()) {
        return false;
    }
    req->old = std::move(*old);
    return del(req->key);
}

// KVTX: pages. Durable pages are read from the snapshot's mmap, while the tx's writes are buffered by page number,
// since a reused page is written in place rather than appended.
const uint8_t* KVTX::page_read(uint64_t ptr) const {
    if (auto it = page_updates_.find(ptr); it != page_updates_.end()) {
        return it->second.data(); // the tx may read back a page it has already written
    }
    return page_get_mapped(ptr);
}

BNode KVTX::page_get(uint64_t ptr) const {
    const uint8_t* page = page_read(ptr);
    return decode(std::vector<uint8_t>(page, page + BTREE_PAGE_SIZE));
}

uint64_t KVTX::page_new(const BNode& node) {
    uint64_t ptr = free_.pop_head();
    if (ptr == 0) {
        ptr = kv_->flushed_ + page_append_;
        ++page_append_;
    }
    page_updates_[ptr] = encode(node);
    return ptr;
}

void KVTX::page_del(uint64_t ptr) {
    free_.push_tail(ptr);
}

uint8_t* KVTX::page_use(uint64_t ptr) {
    auto it = page_updates_.find(ptr);
    if (it == page_updates_.end()) {
        const uint8_t* durable = page_get_mapped(ptr);
        it = page_updates_.emplace(ptr, std::vector<uint8_t>(durable, durable + BTREE_PAGE_SIZE)).first;
    }
    return it->second.data();
}

uint64_t KVTX::page_append() {
    uint64_t ptr = kv_->flushed_ + page_append_;
    ++page_append_;
    page_updates_[ptr] = std::vector<uint8_t>(BTREE_PAGE_SIZE, 0);
    return ptr;
}

// KV: meta page
std::vector<uint8_t> KV::save_meta() const {
    std::vector<uint8_t> data(META_DATA_SIZE, 0);
    std::memcpy(data.data(), DB_SIG, sizeof(DB_SIG) - 1);
    write_u64(data.data() + meta_offset(META_ROOT), root_);
    write_u64(data.data() + meta_offset(META_FLUSHED), flushed_);
    write_u64(data.data() + meta_offset(META_HEAD_PAGE), free_.head_page);
    write_u64(data.data() + meta_offset(META_HEAD_SEQ), free_.head_seq);
    write_u64(data.data() + meta_offset(META_TAIL_PAGE), free_.tail_page);
    write_u64(data.data() + meta_offset(META_TAIL_SEQ), free_.tail_seq);
    write_u64(data.data() + meta_offset(META_VERSION), version_);
    return data;
}

// The meta page isn't written yet for an empty file; it's initialized on
// the 1st commit.
void KV::read_root() {
    struct stat st;
    if (::fstat(fd_, &st) != 0) {
        throw std::runtime_error(std::string("KV: fstat failed: ") + std::strerror(errno));
    }
    if (st.st_size == 0) {
        // Page 0 is the meta page and page 1 the free list's first node, which KV::begin buffers until a commit.
        root_ = 0;
        version_ = 0;
        durable_version_ = 0;
        flushed_ = 2;
        free_ = FreeListState{.head_page = 1, .head_seq = 0, .tail_page = 1, .tail_seq = 0};
        return;
    }
    if (static_cast<uint64_t>(st.st_size) < META_DATA_SIZE) {
        throw std::runtime_error("KV: file too small to contain a meta page");
    }

    std::vector<uint8_t> data(META_DATA_SIZE);
    ssize_t n = ::pread(fd_, data.data(), data.size(), 0);
    if (n != static_cast<ssize_t>(data.size())) {
        throw std::runtime_error("KV: read meta page failed");
    }
    if (std::memcmp(data.data(), DB_SIG, sizeof(DB_SIG) - 1) != 0) {
        throw std::runtime_error("KV: not a valid db file (bad signature)");
    }

    Meta meta = parse_meta(data);
    // A committed file has at least the meta page and 1 free list node, every
    // pointer must fall inside it, and every commit counts in its version.
    bool valid = meta.version != 0 && meta.flushed >= 2 && meta.root < meta.flushed &&
                 meta.free_state.head_page != 0 && meta.free_state.head_page < meta.flushed &&
                 meta.free_state.tail_page != 0 && meta.free_state.tail_page < meta.flushed &&
                 meta.free_state.head_seq <= meta.free_state.tail_seq;
    if (!valid) {
        throw std::runtime_error("KV: corrupt meta page");
    }

    root_ = meta.root;
    version_ = meta.version;
    durable_version_ = meta.version;
    flushed_ = meta.flushed;
    free_ = meta.free_state;
    extend_mmap(flushed_ * BTREE_PAGE_SIZE); // map durable pages so transactions can read them right away
}

void KV::update_root() {
    std::vector<uint8_t> data = save_meta();
    if (::pwrite(fd_, data.data(), data.size(), 0) != static_cast<ssize_t>(data.size())) {
        throw std::runtime_error(std::string("KV: write meta page failed: ") + std::strerror(errno));
    }
}

// KV: transactions
void KV::snapshot(KVReader* tx) {
    tx->version = version_;
    tx->tree_.root = root_;
    tx->chunks_ = chunks_;
}

void KV::begin_read(KVReader* tx) {
    std::lock_guard lock(mu_);
    snapshot(tx);
    tx->tree_.pages = &tx->mapped_;
    readers_.push(tx);
}

void KV::end_read(KVReader* tx) {
    std::lock_guard lock(mu_);
    readers_.remove(tx->heap_index);
}

void KV::begin(KVTX* tx) {
    writer_.lock();
    tx->kv_ = this;
    tx->tree_.pages = &tx->pages_;
    tx->page_updates_.clear();
    tx->page_append_ = 0;
    tx->free_ = FreeList(&tx->pages_, free_);
    tx->free_.version = version_;
    tx->free_.min_reader = durable_version_; // pages freed since the last durable meta page may be in the file's tree
    {
        std::lock_guard lock(mu_);
        snapshot(tx);
        const KVReader* oldest = readers_.min();
        if (oldest != nullptr && version_before(oldest->version, durable_version_)) {
            tx->free_.min_reader = oldest->version; // pages freed at or after its version may be in its tree
        }
    }
    if (version_ == 0) {
        tx->page_updates_[1] = std::vector<uint8_t>(BTREE_PAGE_SIZE, 0); // a new file: the free list's empty first node
    }
}

void KV::abort(KVTX* tx) {
    tx->page_updates_.clear(); // nothing committed references them
    writer_.unlock();
}

// 2-phase commit. Phase 1 never overwrites a page that the committed version or an open reader uses (tree pages are
// copy-on-write and only reused once unreachable, and free list nodes only change past the committed tail), so if
// phase 1 fails, the meta page on disk still points at an intact tree and dropping tx is a complete rollback.
void KV::commit(KVTX* tx) {
    std::unique_lock writer(writer_, std::adopt_lock); // released however the commit ends
    if (tx->tree_.root == root_) {
        return; // no writes: each one moves the root to a newly allocated page
    }

    write_pages(tx);
    if (::fsync(fd_) != 0) {
        throw std::runtime_error(std::string("KV: fsync failed: ") + std::strerror(errno));
    }
    flushed_ += tx->page_append_;
    free_ = tx->free_.state();
    {
        std::lock_guard lock(mu_);
        root_ = tx->tree_.root;
        ++version_;
    }

    // Phase 2 is not rolled back. Once the meta page write or its fsync fails, the file may hold either the old or the
    // new root, and reverting memory to the old version would let the next commit overwrite pages the new one uses.
    // Keeping the new version is safe either way: its pages are durable, and the pages it freed stay unreleased.
    update_root();
    if (::fsync(fd_) != 0) {
        throw std::runtime_error(std::string("KV: fsync failed: ") + std::strerror(errno));
    }
    // This version is durable, so the pages it dropped can now be reused.
    durable_version_ = version_;
}

// Buffered pages mix appends with in-place rewrites of reused pages, so they're
// written by page number, coalescing consecutive runs into one pwritev().
void KV::write_pages(KVTX* tx) {
    extend_mmap((flushed_ + tx->page_append_) * BTREE_PAGE_SIZE);

    auto& updates = tx->page_updates_;
    auto it = updates.begin();
    while (it != updates.end()) {
        uint64_t first = it->first;
        std::vector<iovec> iovs;
        for (uint64_t ptr = first; it != updates.end() && it->first == ptr && iovs.size() < MAX_IOVS; ++it, ++ptr) {
            iovs.push_back(iovec{it->second.data(), it->second.size()});
        }

        off_t offset = static_cast<off_t>(first * BTREE_PAGE_SIZE);
        ssize_t want = static_cast<ssize_t>(iovs.size() * BTREE_PAGE_SIZE);
        if (::pwritev(fd_, iovs.data(), static_cast<int>(iovs.size()), offset) != want) {
            throw std::runtime_error("KV: pwritev failed");
        }
    }
}
