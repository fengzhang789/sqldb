#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "storage/btree.h"
#include "storage/btree_iter.h"
#include "storage/pagemanager.h"

// Existence requirement for KV::update.
enum class UpdateMode {
    UPSERT,       // insert or overwrite
    UPDATE_ONLY,  // fail if the key does not exist
    INSERT_ONLY,  // fail if the key already exists
};

// KV::update's request: key, val and mode in; added and old out.
struct InsertReq {
    std::vector<uint8_t> key;
    std::vector<uint8_t> val;
    UpdateMode mode = UpdateMode::UPSERT;
    bool added = false; // out: the key didn't exist before
    std::vector<uint8_t> old; // out: the previous value, if !added
};

// KV::del's request: key in; old out.
struct DeleteReq {
    std::vector<uint8_t> key;
    std::vector<uint8_t> old; // out: the deleted value
};

// KV is a durable, crash-safe key-value store backed by a copy-on-write
// B+tree persisted to a single file.
// Two phase update: new B+tree pages are fsynced before the root is fsynced
// to make the whole tree atomic.
// Pages dropped by an update are recycled through a free list (see freelist.h)
// whose position is committed in the meta page with the tree root, so a page is
// only reused once the version referencing it has been replaced.
// Note: single-process sequential access
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
        bool update(InsertReq* req); // set() per req->mode; false, writing nothing, if the mode isn't met
        bool del(DeleteReq* req); // del() that also reports the removed value; false, writing nothing, if absent
        BIter seek(const std::vector<uint8_t>& key, CMP cmp) const; // invalidated by any later set()/del()

    private:
        int fd_ = -1;
        BTree tree_;
        bool failed_ = false; // did the last update fail? on-disk meta may not match memory

        std::optional<PageManager> pages_; // backs BTree pages with the on-disk file

        void update_or_revert(const std::vector<uint8_t>& meta); // 2-phase update, reverting to `meta` on failure
        void update_file(); // write, fsync, root, fsync
        void update_root(); // pwrite the meta page; must be atomic
        void read_root(); // read/validate the meta page, or initialize an empty one
        void load_meta(const std::vector<uint8_t>& data); // restore tree_.root/pages_ from meta bytes
        void write_meta_page(const std::vector<uint8_t>& data); // pwrite raw meta bytes at offset 0
        std::vector<uint8_t> save_meta() const; // serialize the meta page
        int create_file_sync(const std::string& path); // create/open file, fsync parent dir
};

