#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "btree.h"

// KV is a durable, crash-safe key-value store backed by a copy-on-write
// B+tree persisted to a single file.
// Two phase update: new B+tree pages are fsynced before the root is fsynced
// to make the whole tree atomic.
// Note: single-process sequential access, and the file is append only
struct KV {
    public:
        std::string path;

        KV() = default;
        explicit KV(std::string path);
        ~KV();
        KV(const KV&) = delete;
        KV& operator=(const KV&) = delete;

        void open(); // open (create if necessary) the backing file
        void close(); // fsync and closes the file
        std::optional<std::vector<uint8_t>> get(const std::vector<uint8_t>& key) const;
        void set(const std::vector<uint8_t>& key, const std::vector<uint8_t>& val);
        bool del(const std::vector<uint8_t>& key);

    private:
        int fd_ = -1;
        BTree tree_;

        // Backs BTree pages with the on-disk file (TODO: wire up)
        struct FilePages {
            int fd = -1;
        };
        FilePages pages_;

        void update_file(); // write, fsync, root, fsync
        void write_pages(); // TODO: implement
        void update_root(); // TODO: implement
        int create_file_sync(const std::string& path); // create/open file, fsync parent dir
};

