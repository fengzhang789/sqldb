#include "freelist.h"

namespace {
    size_t seq_to_idx(uint64_t seq) {
        return static_cast<size_t>(seq % FREE_LIST_CAP);
    }
}

FreeList::FreeList(IFreeListPages* pages, const FreeListState& state)
    : pages_(pages), state_(state), max_seq_(state.tail_seq) {}

// Takes the item `head_seq` points at, moving to the next node once the head
// node is drained. A drained node is always full, so its `next` is set.
FreeList::Popped FreeList::pop() {
    if (state_.head_seq == max_seq_) {
        return {}; // nothing reusable yet
    }

    ConstLNode node(pages_->read_page(state_.head_page));
    Popped popped;
    popped.ptr = node.get_ptr(seq_to_idx(state_.head_seq));
    ++state_.head_seq;

    if (seq_to_idx(state_.head_seq) == 0) {
        popped.drained_head = state_.head_page;
        state_.head_page = node.next();
        assert(state_.head_page != 0);
    }
    return popped;
}

uint64_t FreeList::pop_head() {
    Popped popped = pop();
    if (popped.drained_head != 0) {
        push_tail(popped.drained_head);
    }
    return popped.ptr;
}

void FreeList::push_tail(uint64_t ptr) {
    assert(ptr != 0); // page 0 is the meta page and is never freed

    LNode(pages_->write_page(state_.tail_page)).set_ptr(seq_to_idx(state_.tail_seq), ptr);
    ++state_.tail_seq;
    if (seq_to_idx(state_.tail_seq) != 0) {
        return; // the tail node still has room
    }

    // The tail node is full. Link a new one right away, so the list still has
    // a node once this one is drained as the head.
    // A reused page still holds stale bytes, including a stale `next`, but a
    // node's `next` is only read once it is full, and filling it writes it.
    Popped popped = pop(); // reuse a page from the list itself if possible
    uint64_t next = popped.ptr != 0 ? popped.ptr : pages_->append_page();
    LNode(pages_->write_page(state_.tail_page)).set_next(next);
    state_.tail_page = next;

    if (popped.drained_head != 0) {
        LNode(pages_->write_page(state_.tail_page)).set_ptr(0, popped.drained_head);
        ++state_.tail_seq;
    }
}

void FreeList::release_pending() {
    max_seq_ = state_.tail_seq;
}

void FreeList::revert(const FreeListState& state) {
    state_ = state;
    max_seq_ = state.tail_seq;
}
