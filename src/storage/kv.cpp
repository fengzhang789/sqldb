// Meta page (page 0) layout:
// +-----------+----------+---------------+-----------+----------+-----------+----------+---------+
// | signature | root ptr | flushed pages | head page | head seq | tail page | tail seq | unused  |
// |   (16B)   |   (8B)   |     (8B)      |   (8B)    |   (8B)   |   (8B)    |   (8B)   | (bytes) |
// +-----------+----------+---------------+-----------+----------+-----------+----------+---------+
// The last 4 fields are the free list's position (see freelist.h); committing
// them with the tree root is what makes page reuse crash-safe.
// Updated via a single pwrite() at offset 0, which is expected to be
// power-loss-atomic since it's page-aligned and touches a single sector.

#include "kv.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace {
    constexpr char DB_SIG[] = "DB"; // not compatible between chapters
    constexpr size_t META_SIG_SIZE = 16;

    // The 8-byte fields following the signature, in order.
    enum MetaField : size_t {
        META_ROOT,
        META_FLUSHED,
        META_HEAD_PAGE,
        META_HEAD_SEQ,
        META_TAIL_PAGE,
        META_TAIL_SEQ,
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
    };

    Meta parse_meta(const std::vector<uint8_t>& data) {
        Meta meta;
        meta.root = read_u64(data.data() + meta_offset(META_ROOT));
        meta.flushed = read_u64(data.data() + meta_offset(META_FLUSHED));
        meta.free_state.head_page = read_u64(data.data() + meta_offset(META_HEAD_PAGE));
        meta.free_state.head_seq = read_u64(data.data() + meta_offset(META_HEAD_SEQ));
        meta.free_state.tail_page = read_u64(data.data() + meta_offset(META_TAIL_PAGE));
        meta.free_state.tail_seq = read_u64(data.data() + meta_offset(META_TAIL_SEQ));
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

// KV: public API
std::optional<std::vector<uint8_t>> KV::get(const std::vector<uint8_t>& key) const {
    return tree_.get(key);
}

void KV::set(const std::vector<uint8_t>& key, const std::vector<uint8_t>& val) {
    std::vector<uint8_t> meta = save_meta();
    tree_.insert(key, val); // throws (e.g. length limit) without mutating anything
    update_or_revert(meta);
}

bool KV::del(const std::vector<uint8_t>& key) {
    std::vector<uint8_t> meta = save_meta();
    bool deleted = tree_.remove(key);
    update_or_revert(meta);
    return deleted;
}

BIter KV::seek(const std::vector<uint8_t>& key, CMP cmp) const {
    return tree_.seek(key, cmp);
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
}

// Restores the tree root, the flushed-page count and the free list position
// from previously-saved meta bytes, to revert a failed update.
void KV::load_meta(const std::vector<uint8_t>& data) {
    Meta meta = parse_meta(data);
    tree_.root = meta.root;
    pages_->revert(meta.flushed, meta.free_state);
}

void KV::write_meta_page(const std::vector<uint8_t>& data) {
    if (::pwrite(fd_, data.data(), data.size(), 0) != static_cast<ssize_t>(data.size())) {
        throw std::runtime_error(std::string("KV: write meta page failed: ") + std::strerror(errno));
    }
}

// KV: two-phase update
void KV::update_root() {
    write_meta_page(save_meta());
}

void KV::update_file() {
    pages_->write_pages();
    if (::fsync(fd_) != 0) {
        throw std::runtime_error("KV: fsync failed");
    }
    update_root();
    if (::fsync(fd_) != 0) {
        throw std::runtime_error("KV: fsync failed");
    }
    // This version is durable, so the pages it dropped can now be reused.
    pages_->release_freed_pages();
}

// 2-phase update with revert-on-failure. `meta` is the meta page as of
// before this operation's tree mutation, i.e. the last known-good state.
//
// After an fsync failure, a filesystem's page cache can disagree with what's
// actually on disk, so we don't trust re-reading the meta page to recover -
// instead we revert the in-memory state immediately (reads keep working) and
// mark failed_, since the on-disk meta page is now in an unknown state: it
// may hold the old or the new root. The next update rewrites and fsyncs
// `meta` first, restoring a known-good on-disk state before it proceeds -
// only then is it safe to let new pages reuse the reverted page numbers.
//
// Reverting is enough even though a failed update rewrites pages in place:
// every page it touched was free or unreferenced in `meta`'s version.
void KV::update_or_revert(const std::vector<uint8_t>& meta) {
    try {
        if (failed_) {
            write_meta_page(meta);
            if (::fsync(fd_) != 0) {
                throw std::runtime_error(std::string("KV: fsync failed: ") + std::strerror(errno));
            }
            failed_ = false;
        }
        update_file();
    } catch (...) {
        load_meta(meta);
        failed_ = true;
        throw;
    }
}

