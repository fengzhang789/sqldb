// Meta page (page 0) layout:
// +-----------+----------+---------------+---------+
// | signature | root ptr | flushed pages | unused  |
// |   (16B)   |   (8B)   |     (8B)      | (bytes) |
// +-----------+----------+---------------+---------+
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
    constexpr size_t META_DATA_SIZE = META_SIG_SIZE + 8 + 8; // sig + root ptr + flushed pages

    static_assert(sizeof(DB_SIG) - 1 <= META_SIG_SIZE, "DB_SIG must fit within the signature field");

    void write_u64(uint8_t* ptr, uint64_t val) {
        std::memcpy(ptr, &val, sizeof(val));
    }

    uint64_t read_u64(const uint8_t* ptr) {
        uint64_t val;
        std::memcpy(&val, ptr, sizeof(val));
        return val;
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
    tree_.insert(key, val);
    update_file();
}

bool KV::del(const std::vector<uint8_t>& key) {
    bool deleted = tree_.remove(key);
    update_file();
    return deleted;
}

// KV: meta page
std::vector<uint8_t> KV::save_meta() const {
    std::vector<uint8_t> data(META_DATA_SIZE, 0);
    std::memcpy(data.data(), DB_SIG, sizeof(DB_SIG) - 1);
    write_u64(data.data() + META_SIG_SIZE, tree_.root);
    write_u64(data.data() + META_SIG_SIZE + 8, pages_->flushed_pages());
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

    uint64_t root = read_u64(data.data() + META_SIG_SIZE);
    uint64_t flushed = read_u64(data.data() + META_SIG_SIZE + 8);
    pages_.emplace(fd_, flushed);
    tree_.root = root;
}

// KV: two-phase update
void KV::update_root() {
    std::vector<uint8_t> meta = save_meta();
    if (::pwrite(fd_, meta.data(), meta.size(), 0) != static_cast<ssize_t>(meta.size())) {
        throw std::runtime_error(std::string("KV: write meta page failed: ") + std::strerror(errno));
    }
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
}

