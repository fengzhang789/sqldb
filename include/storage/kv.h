#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "storage/btree.h"
#include "storage/btree_iter.h"
#include "storage/freelist.h"
#include "storage/pagemanager.h"

// Existence requirement for KVTX::update.
enum class UpdateMode {
    UPSERT,       // insert or overwrite
    UPDATE_ONLY,  // fail if the key does not exist
    INSERT_ONLY,  // fail if the key already exists
};

// KVTX::update's request: key, val and mode in; added and old out.
struct InsertReq {
    std::vector<uint8_t> key;
    std::vector<uint8_t> val;
    UpdateMode mode = UpdateMode::UPSERT;
    bool added = false; // out: the key didn't exist before
    std::vector<uint8_t> old; // out: the previous value, if !added
};

// KVTX::del's request: key in; old out.
struct DeleteReq {
    std::vector<uint8_t> key;
    std::vector<uint8_t> old; // out: the deleted value
};

struct KV;

// KVTX is a KV transaction: its writes only change the in-memory tree until KV::commit makes all of them durable at
// once, or KV::abort drops them. Transactions run one at a time: begin the next once this one has ended.
struct KVTX {
    public:
        std::optional<std::vector<uint8_t>> get(const std::vector<uint8_t>& key) const;
        void set(const std::vector<uint8_t>& key, const std::vector<uint8_t>& val);
        bool del(const std::vector<uint8_t>& key);
        bool update(InsertReq* req); // set() per req->mode; false, writing nothing, if the mode isn't met
        bool del(DeleteReq* req); // del() that also reports the removed value; false, writing nothing, if absent
        BIter seek(const std::vector<uint8_t>& key, CMP cmp) const; // invalidated by any later write or the tx's end

    private:
        friend struct KV;
        KV* kv_ = nullptr;
        uint64_t root_ = 0; // tree root at begin
        uint64_t flushed_ = 0; // durable page count at begin
        FreeListState free_state_; // free list position at begin
};

// KV is a durable, crash-safe key-value store backed by a copy-on-write
// B+tree persisted to a single file.
// All reads and writes go through a transaction (KVTX), and only a commit
// writes to the file.
// Two phase commit: new B+tree pages are fsynced before the root is fsynced
// to make the whole transaction atomic.
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

        void begin(KVTX* tx); // start a transaction on the current in-memory version
        void commit(KVTX* tx); // make tx's writes durable; throws on an I/O error (see kv.cpp for what is rolled back)
        void abort(KVTX* tx); // drop tx's writes, without any I/O

    private:
        friend struct KVTX;

        int fd_ = -1;
        BTree tree_;

        std::optional<PageManager> pages_; // backs BTree pages with the on-disk file

        void update_root(); // pwrite the meta page; must be atomic
        void read_root(); // read/validate the meta page, or initialize an empty one
        std::vector<uint8_t> save_meta() const; // serialize the meta page
        int create_file_sync(const std::string& path); // create/open file, fsync parent dir
};
