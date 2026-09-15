#include "storage/freelist.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <vector>

#include <gtest/gtest.h>

namespace {
    // A brand new list: 1 empty node on page 1.
    constexpr FreeListState kFreshList{.head_page = 1, .head_seq = 0, .tail_page = 1, .tail_seq = 0};

    // Page numbers the tests free, kept clear of the ones FakePages appends.
    constexpr uint64_t kFirstFreedPage = 1000;

    // Stands in for the file: any page can be read or written, as every page
    // below the real page manager's flushed count is mapped.
    class FakePages : public IFreeListPages {
        public:
            const uint8_t* read_page(uint64_t ptr) const override {
                auto it = pages_.find(ptr);
                if (it == pages_.end()) {
                    throw std::out_of_range("FakePages: page was never written");
                }
                return it->second.data();
            }

            uint8_t* write_page(uint64_t ptr) override {
                return page(ptr).data();
            }

            uint64_t append_page() override {
                uint64_t ptr = next_ptr_++;
                page(ptr);
                appended_.push_back(ptr);
                return ptr;
            }

            const std::vector<uint64_t>& appended() const { return appended_; }
            bool was_appended(uint64_t ptr) const {
                return std::find(appended_.begin(), appended_.end(), ptr) != appended_.end();
            }

        private:
            std::vector<uint8_t>& page(uint64_t ptr) {
                auto it = pages_.find(ptr);
                if (it == pages_.end()) {
                    it = pages_.emplace(ptr, std::vector<uint8_t>(BTREE_PAGE_SIZE, 0)).first;
                }
                return it->second;
            }

            std::map<uint64_t, std::vector<uint8_t>> pages_{{1, std::vector<uint8_t>(BTREE_PAGE_SIZE, 0)}};
            std::vector<uint64_t> appended_;
            uint64_t next_ptr_ = 2;
    };

    // Frees `count` pages, numbered from kFirstFreedPage + `first`.
    void push_pages(FreeList& list, uint64_t first, uint64_t count) {
        for (uint64_t i = 0; i < count; ++i) {
            list.push_tail(kFirstFreedPage + first + i);
        }
    }

    // Pops until the list runs out, returning everything it handed over.
    std::vector<uint64_t> pop_all(FreeList& list) {
        std::vector<uint64_t> popped;
        while (uint64_t ptr = list.pop_head()) {
            popped.push_back(ptr);
        }
        return popped;
    }
}

// ============================================================================
// LNode: node layout
// ============================================================================
TEST(LNode, WhenANodeIsFullThenItsItemsFillThePageAfterTheHeader) {
    EXPECT_EQ(FREE_LIST_HEADER, 8u);
    EXPECT_EQ(FREE_LIST_CAP, (BTREE_PAGE_SIZE - 8) / 8);
    EXPECT_LE(FREE_LIST_HEADER + FREE_LIST_CAP * 8, BTREE_PAGE_SIZE);
}

TEST(LNode, WhenNextIsSetThenNextReturnsSameValue) {
    std::vector<uint8_t> page(BTREE_PAGE_SIZE, 0);
    LNode node(page.data());

    EXPECT_EQ(node.next(), 0u); // a zeroed page is an empty node with no successor
    node.set_next(42);
    EXPECT_EQ(node.next(), 42u);
}

TEST(LNode, WhenPtrsAreSetThenGetPtrReturnsSameValues) {
    std::vector<uint8_t> page(BTREE_PAGE_SIZE, 0);
    LNode node(page.data());

    node.set_ptr(0, 7);
    node.set_ptr(1, 8);
    node.set_ptr(FREE_LIST_CAP - 1, 9);

    EXPECT_EQ(node.get_ptr(0), 7u);
    EXPECT_EQ(node.get_ptr(1), 8u);
    EXPECT_EQ(node.get_ptr(FREE_LIST_CAP - 1), 9u);
}

TEST(LNode, WhenTheFirstItemIsSetThenTheNextPointerIsUntouched) {
    std::vector<uint8_t> page(BTREE_PAGE_SIZE, 0);
    LNode node(page.data());

    node.set_next(1);
    node.set_ptr(0, 2);

    EXPECT_EQ(node.next(), 1u);
    EXPECT_EQ(node.get_ptr(0), 2u);
}

TEST(LNode, WhenAWritableViewWritesThenAConstViewReadsTheSameBytes) {
    std::vector<uint8_t> page(BTREE_PAGE_SIZE, 0);
    LNode(page.data()).set_ptr(3, 123);
    LNode(page.data()).set_next(456);

    ConstLNode node(page.data());
    EXPECT_EQ(node.get_ptr(3), 123u);
    EXPECT_EQ(node.next(), 456u);
}

// ============================================================================
// FreeList: consuming
// ============================================================================
TEST(FreeList, WhenTheListIsEmptyThenPopHeadReturnsZero) {
    FakePages pages;
    FreeList list(&pages, kFreshList);

    EXPECT_EQ(list.pop_head(), 0u);
    EXPECT_EQ(list.state().head_seq, 0u);
    EXPECT_EQ(list.state().tail_seq, 0u);
}

TEST(FreeList, WhenPagesAreFreedButNotReleasedThenPopHeadReturnsZero) {
    FakePages pages;
    FreeList list(&pages, kFreshList);

    push_pages(list, 0, 3);

    // Those pages still belong to the version being replaced.
    EXPECT_EQ(list.pop_head(), 0u);
    EXPECT_EQ(list.state().tail_seq, 3u);
}

TEST(FreeList, WhenPagesAreReleasedThenPopHeadReturnsThemInFifoOrder) {
    FakePages pages;
    FreeList list(&pages, kFreshList);

    push_pages(list, 0, 3);
    list.release_pending();

    EXPECT_EQ(list.pop_head(), kFirstFreedPage + 0);
    EXPECT_EQ(list.pop_head(), kFirstFreedPage + 1);
    EXPECT_EQ(list.pop_head(), kFirstFreedPage + 2);
    EXPECT_EQ(list.pop_head(), 0u);
}

TEST(FreeList, WhenPagesAreFreedAfterAReleaseThenTheyWaitForTheNextOne) {
    FakePages pages;
    FreeList list(&pages, kFreshList);

    push_pages(list, 0, 1);
    list.release_pending();
    push_pages(list, 1, 1);

    EXPECT_EQ(list.pop_head(), kFirstFreedPage + 0);
    EXPECT_EQ(list.pop_head(), 0u);

    list.release_pending();
    EXPECT_EQ(list.pop_head(), kFirstFreedPage + 1);
}

TEST(FreeList, WhenAPageIsPoppedThenOnlyTheHeadAdvances) {
    FakePages pages;
    FreeList list(&pages, kFreshList);

    push_pages(list, 0, 2);
    list.release_pending();
    list.pop_head();

    EXPECT_EQ(list.state().head_seq, 1u);
    EXPECT_EQ(list.state().tail_seq, 2u);
    EXPECT_EQ(list.state().head_page, 1u);
    EXPECT_EQ(list.state().tail_page, 1u);
}

// ============================================================================
// FreeList: growing and shrinking the list itself
// ============================================================================
TEST(FreeList, WhenTheTailNodeHasRoomThenNoPageIsAppended) {
    FakePages pages;
    FreeList list(&pages, kFreshList);

    push_pages(list, 0, FREE_LIST_CAP - 1);

    EXPECT_TRUE(pages.appended().empty());
    EXPECT_EQ(list.state().tail_page, 1u);
}

TEST(FreeList, WhenTheTailNodeFillsThenItIsLinkedToAnAppendedNode) {
    FakePages pages;
    FreeList list(&pages, kFreshList);

    push_pages(list, 0, FREE_LIST_CAP);

    ASSERT_EQ(pages.appended().size(), 1u);
    uint64_t appended = pages.appended().front();
    EXPECT_EQ(list.state().tail_page, appended);
    EXPECT_EQ(ConstLNode(pages.read_page(1)).next(), appended);
    EXPECT_EQ(list.state().head_page, 1u); // the head still has items to give
}

TEST(FreeList, WhenTheTailNodeFillsAndPagesAreReusableThenNoPageIsAppended) {
    FakePages pages;
    FreeList list(&pages, kFreshList);

    push_pages(list, 0, FREE_LIST_CAP); // fills node 1, appends node 2
    list.release_pending();
    ASSERT_EQ(pages.appended().size(), 1u);

    // Filling node 2 needs a 3rd node, and this time the list has a reusable
    // page of its own to use for it.
    push_pages(list, FREE_LIST_CAP, FREE_LIST_CAP);

    EXPECT_EQ(pages.appended().size(), 1u);
    EXPECT_EQ(list.state().tail_page, kFirstFreedPage + 0);
}

TEST(FreeList, WhenTheHeadNodeIsDrainedThenItIsGivenBackToTheList) {
    FakePages pages;
    FreeList list(&pages, kFreshList);

    push_pages(list, 0, FREE_LIST_CAP + 1); // node 1 is full, 1 item sits in node 2
    list.release_pending();

    for (uint64_t i = 0; i < FREE_LIST_CAP; ++i) {
        ASSERT_EQ(list.pop_head(), kFirstFreedPage + i);
    }
    EXPECT_EQ(list.state().head_page, list.state().tail_page); // node 1 is gone

    // Node 1 was recycled as a freed page, so it comes back out once the update
    // that drained it is released.
    EXPECT_EQ(list.pop_head(), kFirstFreedPage + FREE_LIST_CAP);
    EXPECT_EQ(list.pop_head(), 0u);
    list.release_pending();
    EXPECT_EQ(list.pop_head(), 1u);
}

TEST(FreeList, WhenManyNodesAreUsedThenEveryFreedPageComesBack) {
    FakePages pages;
    FreeList list(&pages, kFreshList);

    const uint64_t count = 3 * FREE_LIST_CAP + 7;
    push_pages(list, 0, count);
    list.release_pending();

    std::vector<uint64_t> popped = pop_all(list);

    ASSERT_GE(popped.size(), count);
    for (uint64_t i = 0; i < count; ++i) {
        EXPECT_EQ(popped[i], kFirstFreedPage + i) << "at item " << i;
    }
    // Anything past them is a list node the list recycled itself.
    for (size_t i = count; i < popped.size(); ++i) {
        EXPECT_TRUE(popped[i] == 1 || pages.was_appended(popped[i])) << popped[i];
    }
}

TEST(FreeList, WhenEveryItemIsConsumedThenOneNodeIsLeft) {
    FakePages pages;
    FreeList list(&pages, kFreshList);

    push_pages(list, 0, 2 * FREE_LIST_CAP);
    list.release_pending();
    pop_all(list);
    list.release_pending();
    pop_all(list);

    EXPECT_NE(list.state().head_page, 0u);
    EXPECT_EQ(list.state().head_page, list.state().tail_page);
    EXPECT_EQ(list.state().head_seq, list.state().tail_seq);
}

// ============================================================================
// FreeList: persisted state
// ============================================================================
TEST(FreeList, WhenAListIsReloadedFromItsStateThenItResumesWhereItLeftOff) {
    FakePages pages;
    FreeListState saved;
    {
        FreeList list(&pages, kFreshList);
        push_pages(list, 0, 4);
        list.release_pending();
        ASSERT_EQ(list.pop_head(), kFirstFreedPage + 0);
        saved = list.state();
    }

    FreeList reopened(&pages, saved);
    EXPECT_EQ(reopened.pop_head(), kFirstFreedPage + 1);
    EXPECT_EQ(reopened.pop_head(), kFirstFreedPage + 2);
    EXPECT_EQ(reopened.pop_head(), kFirstFreedPage + 3);
    EXPECT_EQ(reopened.pop_head(), 0u);
}

TEST(FreeList, WhenAnUpdateIsRevertedThenItsPopAndPushAreUndone) {
    FakePages pages;
    FreeList list(&pages, kFreshList);

    push_pages(list, 0, 2);
    list.release_pending();
    FreeListState committed = list.state();

    // A failed update: it took a page and freed another one.
    ASSERT_EQ(list.pop_head(), kFirstFreedPage + 0);
    push_pages(list, 2, 1);
    list.revert(committed);

    EXPECT_EQ(list.state().head_seq, committed.head_seq);
    EXPECT_EQ(list.state().tail_seq, committed.tail_seq);
    // The page it took is reusable again and the one it freed is forgotten.
    EXPECT_EQ(list.pop_head(), kFirstFreedPage + 0);
    EXPECT_EQ(list.pop_head(), kFirstFreedPage + 1);
    EXPECT_EQ(list.pop_head(), 0u);
}

TEST(FreeList, WhenRevertedToAStateWithUnreleasedItemsThenTheyStayUnreleased) {
    FakePages pages;
    FreeList list(&pages, kFreshList);
    push_pages(list, 0, 2);
    list.release_pending();

    // A commit whose meta page write failed keeps its state, but not the right to reuse the page it freed: the file
    // may still hold the previous version, which uses that page.
    push_pages(list, 2, 1);
    FreeListState unreleased = list.state();

    // The next transaction takes a page, then aborts.
    ASSERT_EQ(list.pop_head(), kFirstFreedPage + 0);
    list.revert(unreleased);

    EXPECT_EQ(pop_all(list), (std::vector<uint64_t>{kFirstFreedPage + 0, kFirstFreedPage + 1}));
}

// ============================================================================
// FreeList: recirculation over many updates
// ============================================================================
// Mimics a long run of updates, each freeing and taking a few pages.
TEST(FreeListStress, WhenPagesRecirculateThenNoneIsHandedOutTwice) {
    FakePages pages;
    FreeList list(&pages, kFreshList);

    std::set<uint64_t> reusable; // freed and released, so the list owes them back
    std::set<uint64_t> in_use;
    std::vector<uint64_t> freed_this_update;
    uint64_t next_fresh_page = kFirstFreedPage;
    std::mt19937 rng(20250911);

    for (int update = 0; update < 2000; ++update) {
        size_t count = rng() % 4 + 1;
        for (size_t i = 0; i < count; ++i) {
            uint64_t ptr = next_fresh_page++;
            if (!in_use.empty() && rng() % 2 == 0) { // usually free a page back
                auto it = in_use.begin();
                std::advance(it, rng() % in_use.size());
                ptr = *it;
                in_use.erase(it);
            }
            list.push_tail(ptr);
            freed_this_update.push_back(ptr);
        }

        for (size_t i = 0; i < count; ++i) {
            uint64_t ptr = list.pop_head();
            if (ptr == 0) {
                continue;
            }
            // Either a page the test freed, or one the list appended for its own
            // nodes and later recycled.
            EXPECT_TRUE(reusable.erase(ptr) == 1 || pages.was_appended(ptr) || ptr == 1)
                << "page " << ptr << " was not free";
            EXPECT_TRUE(in_use.insert(ptr).second) << "page " << ptr << " was handed out twice";
        }

        reusable.insert(freed_this_update.begin(), freed_this_update.end());
        freed_this_update.clear();
        list.release_pending();
    }

    // The list kept up with the churn instead of hoarding pages.
    EXPECT_LE(pages.appended().size(), reusable.size() / FREE_LIST_CAP + 2);
}
