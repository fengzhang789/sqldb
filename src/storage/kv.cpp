// Meta page (page 0) layout: signature (16B) | root ptr (8B) | flushed pages (8B) | unused
// Written via a single pwrite() so the root pointer update is atomic.

#include "kv.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <filesystem>
#include <stdexcept>

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

// TODO: implement meta-page read/validation and root restoration
void KV::open() {
    fd_ = create_file_sync(path);
    pages_.fd = fd_;
}

void KV::close() {
    if (fd_ < 0) {
        return;
    }
    ::fsync(fd_);
    ::close(fd_);
    fd_ = -1;
    pages_.fd = -1;
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

// KV: two-phase update
// TODO: implement
void KV::write_pages() {
    return;
}

// TODO: implement
void KV::update_root() {
    return;
}

void KV::update_file() {
    write_pages();
    if (::fsync(fd_) != 0) {
        throw std::runtime_error("KV: fsync failed");
    }
    update_root();
    if (::fsync(fd_) != 0) {
        throw std::runtime_error("KV: fsync failed");
    }
}

