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
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace {
    constexpr char DB_SIG[] = "DB";
    constexpr size_t META_SIG_SIZE = 16;

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
    tree_.pages = &*pages_;
}

void KV::close() {
    if (fd_ < 0) {
        return;
    }
    ::fsync(fd_);
    ::close(fd_);
    fd_ = -1;
    pages_.reset();
}

// KVTX: reads and writes on the live in-memory tree
std::optional<std::vector<uint8_t>> KVTX::get(const std::vector<uint8_t>& key) const {
    return kv_->tree_.get(key);
}

void KVTX::set(const std::vector<uint8_t>& key, const std::vector<uint8_t>& val) {
    kv_->tree_.insert(key, val); // throws (e.g. length limit) without mutating anything
}

bool KVTX::del(const std::vector<uint8_t>& key) {
    return kv_->tree_.remove(key);
}

bool KVTX::update(InsertReq* req) {
    std::optional<std::vector<uint8_t>> old = kv_->tree_.get(req->key);
    req->added = !old.has_value();
    req->old = std::move(old).value_or(std::vector<uint8_t>{});
    if ((req->mode == UpdateMode::INSERT_ONLY && !req->added) || (req->mode == UpdateMode::UPDATE_ONLY && req->added)) {
        return false;
    }
    set(req->key, req->val);
    return true;
}

bool KVTX::del(DeleteReq* req) {
    std::optional<std::vector<uint8_t>> old = kv_->tree_.get(req->key);
    if (!old.has_value()) {
        return false;
    }
    req->old = std::move(*old);
    return del(req->key);
}

BIter KVTX::seek(const std::vector<uint8_t>& key, CMP cmp) const {
    return kv_->tree_.seek(key, cmp);
}

// KV: meta page
std::vector<uint8_t> KV::save_meta() const {
    const FreeListState& fl = pages_->free_state();

    std::vector<uint8_t> data(META_DATA_SIZE, 0);
    std::memcpy(data.data(), DB_SIG, sizeof(DB_SIG) - 1);
    write_u64(data.data() + meta_offset(META_ROOT), tree_.root);
    write_u64(data.data() + meta_offset(META_FLUSHED), pages_->flushed_pages());
    write_u64(data.data() + meta_offset(META_HEAD_PAGE), fl.head_page);
    write_u64(data.data() + meta_offset(META_HEAD_SEQ), fl.head_seq);
    write_u64(data.data() + meta_offset(META_TAIL_PAGE), fl.tail_page);
    write_u64(data.data() + meta_offset(META_TAIL_SEQ), fl.tail_seq);
    write_u64(data.data() + meta_offset(META_VERSION), version_);
    return data;
}

// The meta page isn't written yet for an empty file; it's initialized on
// the 1st update_root().
void KV::read_root() {
    struct stat st;
    if (::fstat(fd_, &st) != 0) {
        throw std::runtime_error(std::string("KV: fstat failed: ") + std::strerror(errno));
    }
    if (st.st_size == 0) {
        pages_.emplace(fd_);
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
    // A committed file has at least the meta page and 1 free list node, and
    // every pointer must fall inside it.
    bool valid = meta.flushed >= 2 && meta.root < meta.flushed &&
                 meta.free_state.head_page != 0 && meta.free_state.head_page < meta.flushed &&
                 meta.free_state.tail_page != 0 && meta.free_state.tail_page < meta.flushed &&
                 meta.free_state.head_seq <= meta.free_state.tail_seq;
    if (!valid) {
        throw std::runtime_error("KV: corrupt meta page");
    }

    pages_.emplace(fd_, meta.flushed, meta.free_state);
    tree_.root = meta.root;
    version_ = meta.version;
    durable_version_ = meta.version;
}

void KV::update_root() {
    std::vector<uint8_t> data = save_meta();
    if (::pwrite(fd_, data.data(), data.size(), 0) != static_cast<ssize_t>(data.size())) {
        throw std::runtime_error(std::string("KV: write meta page failed: ") + std::strerror(errno));
    }
}

// KV: transactions
void KV::begin(KVTX* tx) {
    tx->kv_ = this;
    tx->root_ = tree_.root;
    tx->flushed_ = pages_->flushed_pages();
    tx->free_state_ = pages_->free_state();
    // Pages freed since the last durable meta page may still be in the tree the file points at.
    pages_->set_versions(version_, durable_version_);
}

void KV::abort(KVTX* tx) {
    tree_.root = tx->root_;
    if (tx->root_ == 0) {
        pages_.emplace(fd_); // nothing committed yet: back to a new file, whose free list node is only buffered
        return;
    }
    pages_->revert(tx->flushed_, tx->free_state_);
}

// 2-phase commit. Phase 1 never overwrites a page the committed version uses (its tree pages are copy-on-write, and
// free list nodes only change past the committed tail), so if phase 1 fails, the meta page on disk still points at an
// intact tree and aborting tx is a complete rollback.
void KV::commit(KVTX* tx) {
    if (tree_.root == tx->root_) {
        return; // no writes: each one moves the root to a newly allocated page
    }

    try {
        pages_->write_pages();
        if (::fsync(fd_) != 0) {
            throw std::runtime_error(std::string("KV: fsync failed: ") + std::strerror(errno));
        }
    } catch (...) {
        abort(tx);
        throw;
    }
    ++version_;

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
