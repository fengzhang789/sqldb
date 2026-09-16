#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "storage/btree.h"
#include "storage/btree_iter.h"
#include "storage/freelist.h"
#include "storage/kv_reader.h"
#include "storage/pagemanager.h"
#include "storage/reader_heap.h"

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

// KVTX is a KV write transaction. It reads like a KVReader of the version it began on, plus its own writes, which only
// change pages buffered in the tx until KV::commit makes all of them durable at once, or KV::abort drops them.
// Write transactions run one at a time: KV::begin waits for the running one to end, and a tx must end on the thread
// that began it.
struct KVTX : KVReader {
    public:
        void set(const std::vector<uint8_t>& key, const std::vector<uint8_t>& val);
        bool del(const std::vector<uint8_t>& key);
        bool update(InsertReq* req); // set() per req->mode; false, writing nothing, if the mode isn't met
        bool del(DeleteReq* req); // del() that also reports the removed value; false, writing nothing, if absent

    private:
        friend struct KV;
        friend struct KVTXTestPeer; // lets tests check which pages the free list hands out

        // Routes the tree's and the free list's page access to this tx's page_* methods.
        struct Pages : IPageManager, IFreeListPages {
            explicit Pages(KVTX* tx) : tx(tx) {}
            BNode get(uint64_t ptr) const override { return tx->page_get(ptr); }
            uint64_t new_page(const BNode& node) override { return tx->page_new(node); }
            void del(uint64_t ptr) override { tx->page_del(ptr); }
            const uint8_t* read_page(uint64_t ptr) const override { return tx->page_read(ptr); }
            uint8_t* write_page(uint64_t ptr) override { return tx->page_use(ptr); }
            uint64_t append_page() override { return tx->page_append(); }
            KVTX* tx;
        };

        BNode page_get(uint64_t ptr) const;
        uint64_t page_new(const BNode& node); // reuse a free page, else append
        void page_del(uint64_t ptr); // hand the page to the free list
        const uint8_t* page_read(uint64_t ptr) const; // a buffered page shadows the durable one
        uint8_t* page_use(uint64_t ptr); // buffer to update a page in place
        uint64_t page_append(); // append a zeroed page, returning its number

        KV* kv_ = nullptr;
        Pages pages_{this};
        std::map<uint64_t, std::vector<uint8_t>> page_updates_; // buffered writes, by page number
        uint64_t page_append_ = 0; // pages this tx appends past KV's flushed count
        FreeList free_{&pages_, FreeListState{}};
};

// KV is a durable, crash-safe key-value store backed by a copy-on-write
// B+tree persisted to a single file.
// Reads go through a KVReader or a KVTX and writes through a KVTX; only a
// commit writes to the file.
// Two phase commit: new B+tree pages are fsynced before the root is fsynced
// to make the whole transaction atomic.
// Pages dropped by an update are recycled through a free list (see freelist.h)
// whose position is committed in the meta page with the tree root and version,
// so a page is only reused once the version referencing it is durably replaced
// and no open reader can reach it.
// Concurrency: readers run alongside each other and alongside one write
// transaction at a time. Each commit publishes a new version, and a reader
// keeps seeing the version it began on.
struct KV {
    public:
        std::string path;

        KV() = default;
        explicit KV(std::string path);
        ~KV();
        KV(const KV&) = delete;
        KV& operator=(const KV&) = delete;

        void open(); // open (create if necessary) the backing file
        void close(); // fsync and closes the file; every reader and tx must have ended

        void begin_read(KVReader* tx); // start reading the latest version
        void end_read(KVReader* tx);

        void begin(KVTX* tx); // start a write transaction on the latest version, once the running one ends
        void commit(KVTX* tx); // end tx, making its writes durable; throws on an I/O error (see kv.cpp for what is rolled back)
        void abort(KVTX* tx); // end tx, dropping its writes, without any I/O

    private:
        friend struct KVTX;

        int fd_ = -1;
        std::mutex writer_; // held from begin to commit or abort
        std::mutex mu_; // guards what readers share with the writer: the 4 fields below

        uint64_t root_ = 0; // the latest version's tree root
        uint64_t version_ = 0; // commits so far; the running tx's freed pages are recorded with it
        std::vector<MmapChunk> chunks_; // grows at commits, unmapped at close
        ReaderHeap readers_;

        // Only write transactions use these, so writer_ guards them.
        uint64_t flushed_ = 0; // pages on disk
        FreeListState free_; // the free list's committed position
        uint64_t durable_version_ = 0; // version_ as of the last meta page known to be durable
        size_t mmap_total_ = 0; // total mapped bytes, can exceed the file size

        void snapshot(KVReader* tx); // copy the latest version into tx; requires mu_
        void extend_mmap(size_t size); // grow the mmap so it covers at least `size` bytes
        void write_pages(KVTX* tx); // write the pages tx buffered
        void update_root(); // pwrite the meta page; must be atomic
        void read_root(); // read/validate the meta page, or initialize an empty one
        std::vector<uint8_t> save_meta() const; // serialize the meta page
        int create_file_sync(const std::string& path); // create/open file, fsync parent dir
};
