#include "storage/freelist.h"

namespace {
    size_t seq_to_idx(uint64_t seq) {
        return static_cast<size_t>(seq % FREE_LIST_CAP);
    }
}

FreeList::FreeList(IFreeListPages* pages, const FreeListState& state) : pages_(pages), state_(state) {}

// Takes the item `head_seq` points at if no reader can still see it, moving to
// the next node once the head node is drained. A drained node is always full,
// so its `next` is set.
FreeList::Popped FreeList::pop() {
    if (state_.head_seq == state_.tail_seq) {
        return {}; // empty
    }

    ConstLNode node(pages_->read_page(state_.head_page));
    size_t idx = seq_to_idx(state_.head_seq);
    if (!version_before(node.get_version(idx), min_reader)) {
        return {}; // still visible, and every item behind it was freed no earlier
    }

    Popped popped;
    popped.ptr = node.get_ptr(idx);
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
    assert(!version_before(version, min_reader)); // else an update could reuse a page it just freed

    append_item(ptr);
    if (seq_to_idx(state_.tail_seq) != 0) {
        return; // the tail node still has room
    }

    // The tail node is full. Link a new one right away, so the list still has
    // a node once this one is drained as the head.
    // A reused page still holds stale bytes, including a stale `next`, but a
    // node's `next` is only read once it is full, and filling it writes it.
    // The pop can't take the item just appended, or drain the tail node, since that item was freed at `version`.
    Popped popped = pop(); // reuse a page from the list itself if possible
    uint64_t next = popped.ptr != 0 ? popped.ptr : pages_->append_page();
    LNode(pages_->write_page(state_.tail_page)).set_next(next);
    state_.tail_page = next;

    if (popped.drained_head != 0) {
        append_item(popped.drained_head);
    }
}

void FreeList::append_item(uint64_t ptr) {
    size_t idx = seq_to_idx(state_.tail_seq);
    LNode node(pages_->write_page(state_.tail_page));
    node.set_ptr(idx, ptr);
    node.set_version(idx, version);
    ++state_.tail_seq;
}

// Items pushed before `state` that aren't reusable yet stay that way, since that only depends on their version: a
// commit that failed after writing its pages keeps its state, yet the file may still hold the previous version, which
// uses the pages that commit freed.
void FreeList::revert(const FreeListState& state) {
    state_ = state;
}
