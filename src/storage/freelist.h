#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "btree.h" // BTREE_PAGE_SIZE

// Free list node layout (one page):
// +--------+---------------+---------+
// |  next  |   pointers    | unused  |
// |  (8B)  |    (n * 8B)   | (bytes) |
// +--------+---------------+---------+
constexpr size_t FREE_LIST_HEADER = 8;
constexpr size_t FREE_LIST_CAP = (BTREE_PAGE_SIZE - FREE_LIST_HEADER) / 8;

// LNodeT is a non-owning view of one free list node's page bytes: LNode over a
// writable page, ConstLNode over a read-only one. The page must outlive it.
//
// Unlike a BNode, a list node is updated in place. That stays crash-safe
// because an update only writes the `next` pointer of a full node or a slot
// past the last live item, never data the committed meta page points at.
template <typename Byte>
class LNodeT {
    static_assert(std::is_same_v<std::remove_const_t<Byte>, uint8_t>);

    public:
        explicit LNodeT(Byte* page) : page_(page) {}

        uint64_t next() const { return load(0); }
        uint64_t get_ptr(size_t idx) const { return load(slot(idx)); }

        void set_next(uint64_t ptr)
            requires(!std::is_const_v<Byte>)
        {
            store(0, ptr);
        }

        void set_ptr(size_t idx, uint64_t ptr)
            requires(!std::is_const_v<Byte>)
        {
            store(slot(idx), ptr);
        }

    private:
        static size_t slot(size_t idx) {
            assert(idx < FREE_LIST_CAP);
            return FREE_LIST_HEADER + idx * 8;
        }

        uint64_t load(size_t offset) const {
            uint64_t val;
            std::memcpy(&val, page_ + offset, sizeof(val));
            return val;
        }

        void store(size_t offset, uint64_t val)
            requires(!std::is_const_v<Byte>)
        {
            std::memcpy(page_ + offset, &val, sizeof(val));
        }

        Byte* page_;
};

using LNode = LNodeT<uint8_t>;
using ConstLNode = LNodeT<const uint8_t>;

// The part of the free list persisted in the KV meta page, and so the part
// that is rolled back with it.
//
// The seqs are monotonic counters rather than slot indexes: seq % FREE_LIST_CAP
// is the slot within a node, while the counter itself is a unique position in
// the list, so comparing 2 counters tells which comes first.
struct FreeListState {
    uint64_t head_page = 0; // items are consumed here
    uint64_t head_seq = 0;
    uint64_t tail_page = 0; // items are appended here
    uint64_t tail_seq = 0;
};

// How the free list reaches its own pages. Separate from IPageManager because
// the free list stores raw page numbers rather than B+tree nodes, and updates
// its pages in place instead of copy-on-write.
struct IFreeListPages {
    virtual ~IFreeListPages() = default;
    virtual const uint8_t* read_page(uint64_t ptr) const = 0;
    virtual uint8_t* write_page(uint64_t ptr) = 0; // buffer to update a page in place
    virtual uint64_t append_page() = 0; // append a zeroed page, returning its number
};

// FreeList holds the page numbers that are free to reuse. It is an unrolled
// linked list of pages, which lets it manage its own space: a node it needs is
// taken from itself before resorting to appending, and a node it drains is
// given back to itself.
//
// A page freed during an update still belongs to the committed version of the
// tree, so pushed items are only handed back out once release_pending() marks
// that update durable.
class FreeList {
    public:
        FreeList(IFreeListPages* pages, const FreeListState& state);

        uint64_t pop_head(); // take a reusable page, or 0 if there is none
        void push_tail(uint64_t ptr); // hand a freed page over for later reuse

        void release_pending(); // make items added since the last call reusable
        void revert(const FreeListState& state); // roll back to a persisted state

        const FreeListState& state() const { return state_; }

    private:
        // A popped item, plus the head node it drained (0 if that node still
        // holds items). A drained node is itself a reusable page.
        struct Popped {
            uint64_t ptr = 0;
            uint64_t drained_head = 0;
        };

        Popped pop(); // take an item without recycling the drained head node

        IFreeListPages* pages_;
        FreeListState state_;
        uint64_t max_seq_; // items at or past this seq are not reusable yet
};
