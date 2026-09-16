#include "storage/freelist.h"

#include <algorithm>
#include <cstdint>
#include <limits>
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
    // below KV's flushed count is mapped.
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

    // Ends the running update durably with no reader open: every page freed so far becomes reusable, and pages freed
    // from now on belong to the next version.
    void end_update(FreeList& list) {
        ++list.version;
        list.min_reader = list.version;
    }
}

// ============================================================================
// LNode: node layout
// ============================================================================
TEST(LNode, WhenANodeIsFullThenItsItemsFillThePageAfterTheHeader) {
    EXPECT_EQ(FREE_LIST_HEADER, 8u);
    EXPECT_EQ(FREE_LIST_ITEM_SIZE, 16u);
    EXPECT_EQ(FREE_LIST_CAP, (BTREE_PAGE_SIZE - 8) / 16);
    EXPECT_LE(FREE_LIST_HEADER + FREE_LIST_CAP * FREE_LIST_ITEM_SIZE, BTREE_PAGE_SIZE);
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

TEST(LNode, WhenVersionsAreSetThenEachStaysPairedWithItsPtr) {
    std::vector<uint8_t> page(BTREE_PAGE_SIZE, 0);
    LNode node(page.data());

    for (size_t idx : {size_t{0}, size_t{1}, FREE_LIST_CAP - 1}) {
        node.set_ptr(idx, 10 + idx);
        node.set_version(idx, 100 + idx);
    }

    for (size_t idx : {size_t{0}, size_t{1}, FREE_LIST_CAP - 1}) {
        EXPECT_EQ(node.get_ptr(idx), 10 + idx) << "slot " << idx;
        EXPECT_EQ(node.get_version(idx), 100 + idx) << "slot " << idx;
    }
}

TEST(LNode, WhenTheFirstItemIsSetThenTheNextPointerIsUntouched) {
    std::vector<uint8_t> page(BTREE_PAGE_SIZE, 0);
    LNode node(page.data());

    node.set_next(1);
    node.set_ptr(0, 2);
    node.set_version(0, 3);

    EXPECT_EQ(node.next(), 1u);
    EXPECT_EQ(node.get_ptr(0), 2u);
}

TEST(LNode, WhenAWritableViewWritesThenAConstViewReadsTheSameBytes) {
    std::vector<uint8_t> page(BTREE_PAGE_SIZE, 0);
    LNode(page.data()).set_ptr(3, 123);
    LNode(page.data()).set_version(3, 789);
    LNode(page.data()).set_next(456);

    ConstLNode node(page.data());
    EXPECT_EQ(node.get_ptr(3), 123u);
    EXPECT_EQ(node.get_version(3), 789u);
    EXPECT_EQ(node.next(), 456u);
}

// ============================================================================
// version_before
// ============================================================================
TEST(VersionBefore, WhenAVersionIsOlderThenItIsBefore) {
    EXPECT_TRUE(version_before(4, 5));
    EXPECT_FALSE(version_before(5, 5));
    EXPECT_FALSE(version_before(6, 5));
}

TEST(VersionBefore, WhenTheCounterWrapsAroundThenTheVersionBeforeTheWrapIsStillBefore) {
    constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();

    EXPECT_TRUE(version_before(kMax, 0));
    EXPECT_TRUE(version_before(kMax - 1, 1));
    EXPECT_FALSE(version_before(0, kMax));
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

TEST(FreeList, WhenPagesAreFreedByTheRunningUpdateThenPopHeadReturnsZero) {
    FakePages pages;
    FreeList list(&pages, kFreshList);

    push_pages(list, 0, 3);

    // Those pages still belong to the version being replaced.
    EXPECT_EQ(list.pop_head(), 0u);
    EXPECT_EQ(list.state().tail_seq, 3u);
}

TEST(FreeList, WhenTheUpdateEndsThenPopHeadReturnsItsFreedPagesInFifoOrder) {
    FakePages pages;
    FreeList list(&pages, kFreshList);

    push_pages(list, 0, 3);
    end_update(list);

    EXPECT_EQ(list.pop_head(), kFirstFreedPage + 0);
    EXPECT_EQ(list.pop_head(), kFirstFreedPage + 1);
    EXPECT_EQ(list.pop_head(), kFirstFreedPage + 2);
    EXPECT_EQ(list.pop_head(), 0u);
}

TEST(FreeList, WhenPagesAreFreedByTheNextUpdateThenTheyWaitForItToEnd) {
    FakePages pages;
    FreeList list(&pages, kFreshList);

    push_pages(list, 0, 1);
    end_update(list);
    push_pages(list, 1, 1);

    EXPECT_EQ(list.pop_head(), kFirstFreedPage + 0);
    EXPECT_EQ(list.pop_head(), 0u);

    end_update(list);
    EXPECT_EQ(list.pop_head(), kFirstFreedPage + 1);
}

TEST(FreeList, WhenAPageIsPoppedThenOnlyTheHeadAdvances) {
    FakePages pages;
    FreeList list(&pages, kFreshList);

    push_pages(list, 0, 2);
    end_update(list);
    list.pop_head();

    EXPECT_EQ(list.state().head_seq, 1u);
    EXPECT_EQ(list.state().tail_seq, 2u);
    EXPECT_EQ(list.state().head_page, 1u);
    EXPECT_EQ(list.state().tail_page, 1u);
}

// ============================================================================
// FreeList: versions and readers
// ============================================================================
TEST(FreeList, WhenAPageIsFreedThenItsNodeRecordsTheFreeingVersion) {
    FakePages pages;
    FreeList list(&pages, kFreshList);
    list.version = 7;
    list.min_reader = 7;

    push_pages(list, 0, 1);

    ConstLNode node(pages.read_page(1));
    EXPECT_EQ(node.get_ptr(0), kFirstFreedPage);
    EXPECT_EQ(node.get_version(0), 7u);
}

TEST(FreeList, WhenAPageWasFreedAtOrAfterMinReaderThenPopHeadRefusesIt) {
    FakePages pages;
    FreeList list(&pages, kFreshList);
    list.version = 5;
    list.min_reader = 5;
    push_pages(list, 0, 1);

    // A later update, while the oldest open reader began at or before version 5 and may still see the page.
    list.version = 8;
    for (uint64_t min_reader : {3, 5}) {
        list.min_reader = min_reader;
        EXPECT_EQ(list.pop_head(), 0u) << "min_reader " << min_reader;
        EXPECT_EQ(list.state().head_seq, 0u) << "a refused page stays at the head";
    }
}

TEST(FreeList, WhenMinReaderMovesPastTheFreeingVersionThenPopHeadHandsThePageOut) {
    FakePages pages;
    FreeList list(&pages, kFreshList);
    list.version = 5;
    list.min_reader = 5;
    push_pages(list, 0, 1);

    list.version = 8;
    list.min_reader = 5;
    ASSERT_EQ(list.pop_head(), 0u);

    list.min_reader = 6; // the reader of version 5 ended; the oldest one left began at 6
    EXPECT_EQ(list.pop_head(), kFirstFreedPage);
}

TEST(FreeList, WhenMinReaderFallsBetweenVersionsThenOnlyThePagesFreedBeforeItAreHandedOut) {
    FakePages pages;
    FreeList list(&pages, kFreshList);
    list.min_reader = 1;
    for (uint64_t version = 1; version <= 3; ++version) {
        list.version = version;
        push_pages(list, 2 * (version - 1), 2);
    }

    list.version = 4;
    list.min_reader = 3;
    EXPECT_EQ(pop_all(list), (std::vector<uint64_t>{kFirstFreedPage + 0, kFirstFreedPage + 1, kFirstFreedPage + 2,
                                                    kFirstFreedPage + 3}));

    list.min_reader = 4;
    EXPECT_EQ(pop_all(list), (std::vector<uint64_t>{kFirstFreedPage + 4, kFirstFreedPage + 5}));
}

TEST(FreeList, WhenUpdatesFreeAndTakePagesRepeatedlyThenPagesComeBackInTheOrderTheyWereFreed) {
    FakePages pages;
    FreeList list(&pages, kFreshList);

    std::vector<uint64_t> popped;
    for (uint64_t update = 0; update < 5; ++update) {
        push_pages(list, 3 * update, 3);
        if (uint64_t ptr = list.pop_head()) {
            popped.push_back(ptr);
        }
        end_update(list);
    }
    for (uint64_t ptr : pop_all(list)) {
        popped.push_back(ptr);
    }

    std::vector<uint64_t> freed;
    for (uint64_t i = 0; i < 15; ++i) {
        freed.push_back(kFirstFreedPage + i);
    }
    EXPECT_EQ(popped, freed);
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
    end_update(list);
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
    end_update(list);

    for (uint64_t i = 0; i < FREE_LIST_CAP; ++i) {
        ASSERT_EQ(list.pop_head(), kFirstFreedPage + i);
    }
    EXPECT_EQ(list.state().head_page, list.state().tail_page); // node 1 is gone

    // Node 1 was recycled as a page freed by the running update, so it comes
    // back out once that update ends.
    EXPECT_EQ(list.pop_head(), kFirstFreedPage + FREE_LIST_CAP);
    EXPECT_EQ(list.pop_head(), 0u);
    end_update(list);
    EXPECT_EQ(list.pop_head(), 1u);
}

TEST(FreeList, WhenManyNodesAreUsedThenEveryFreedPageComesBack) {
    FakePages pages;
    FreeList list(&pages, kFreshList);

    const uint64_t count = 3 * FREE_LIST_CAP + 7;
    push_pages(list, 0, count);
    end_update(list);

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
    end_update(list);
    pop_all(list);
    end_update(list);
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
    uint64_t version = 0;
    {
        FreeList list(&pages, kFreshList);
        push_pages(list, 0, 4);
        end_update(list);
        ASSERT_EQ(list.pop_head(), kFirstFreedPage + 0);
        saved = list.state();
        version = list.version;
    }

    FreeList reopened(&pages, saved);
    reopened.version = version; // KV restores the version from the meta page along with the state
    reopened.min_reader = version;
    EXPECT_EQ(reopened.pop_head(), kFirstFreedPage + 1);
    EXPECT_EQ(reopened.pop_head(), kFirstFreedPage + 2);
    EXPECT_EQ(reopened.pop_head(), kFirstFreedPage + 3);
    EXPECT_EQ(reopened.pop_head(), 0u);
}

TEST(FreeList, WhenAnUpdateIsRevertedThenItsPopAndPushAreUndone) {
    FakePages pages;
    FreeList list(&pages, kFreshList);

    push_pages(list, 0, 2);
    end_update(list);
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

TEST(FreeList, WhenRevertedToAStateWithItemsNotYetReusableThenTheyStayThatWay) {
    FakePages pages;
    FreeList list(&pages, kFreshList);
    push_pages(list, 0, 2);
    end_update(list);

    // A commit whose meta page write failed keeps its state and moves on to the next version, but min_reader stays:
    // the file may still hold the previous version, which uses the page it freed.
    push_pages(list, 2, 1);
    ++list.version;
    FreeListState kept = list.state();

    // The next transaction takes a page, then aborts.
    ASSERT_EQ(list.pop_head(), kFirstFreedPage + 0);
    list.revert(kept);

    EXPECT_EQ(pop_all(list), (std::vector<uint64_t>{kFirstFreedPage + 0, kFirstFreedPage + 1}));
}

// ============================================================================
// FreeList: recirculation over many updates
// ============================================================================
// Mimics a long run of updates, each freeing and taking a few pages.
TEST(FreeListStress, WhenPagesRecirculateThenNoneIsHandedOutTwice) {
    FakePages pages;
    FreeList list(&pages, kFreshList);

    std::set<uint64_t> reusable; // freed by an update that ended, so the list owes them back
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
        end_update(list);
    }

    // The list kept up with the churn instead of hoarding pages.
    EXPECT_LE(pages.appended().size(), reusable.size() / FREE_LIST_CAP + 2);
}

// Mimics updates running while readers of older versions come and go: a page must not be handed out while a reader
// that began at or before the version that freed it is still open.
TEST(FreeListStress, WhenReadersPinOldVersionsThenNoPageTheyCouldSeeIsHandedOut) {
    constexpr uint64_t kUpdates = 2000;
    FakePages pages;
    FreeList list(&pages, kFreshList);

    std::map<uint64_t, uint64_t> freed_at; // freed page -> the version that freed it, until it is handed back out
    std::multiset<uint64_t> readers; // the versions open readers began at
    std::set<uint64_t> in_use;
    uint64_t next_fresh_page = kFirstFreedPage;
    std::mt19937 rng(20260915);

    for (uint64_t version = 0; version < kUpdates; ++version) {
        if (rng() % 3 == 0) {
            readers.insert(version); // a reader begins at the latest version
        }
        if (!readers.empty() && rng() % 3 == 0) {
            auto it = readers.begin();
            std::advance(it, rng() % readers.size());
            readers.erase(it);
        }
        list.version = version;
        list.min_reader = readers.empty() ? version : *readers.begin();

        size_t count = rng() % 4 + 1;
        for (size_t i = 0; i < count; ++i) {
            uint64_t ptr = next_fresh_page++;
            if (!in_use.empty() && rng() % 2 == 0) {
                auto it = in_use.begin();
                std::advance(it, rng() % in_use.size());
                ptr = *it;
                in_use.erase(it);
            }
            list.push_tail(ptr);
            freed_at[ptr] = version;
        }

        for (size_t i = 0; i < count; ++i) {
            uint64_t ptr = list.pop_head();
            if (ptr < kFirstFreedPage) {
                continue; // nothing reusable, or a list node the list recycled itself
            }
            auto it = freed_at.find(ptr);
            ASSERT_NE(it, freed_at.end()) << "page " << ptr << " was not free";
            ASSERT_TRUE(version_before(it->second, list.min_reader))
                << "page " << ptr << " freed at version " << it->second << " was handed out with min_reader "
                << list.min_reader;
            freed_at.erase(it);
            in_use.insert(ptr);
        }
    }

    // With every reader gone, everything freed before the next update comes back, except for the pages the list holds
    // itself: its linked nodes, and drained nodes given back at version kUpdates.
    list.version = kUpdates;
    list.min_reader = kUpdates;
    for (uint64_t ptr : pop_all(list)) {
        freed_at.erase(ptr);
    }

    const FreeListState& state = list.state();
    std::set<uint64_t> held{state.head_page, state.tail_page};
    uint64_t node = state.head_page;
    for (uint64_t seq = state.head_seq; seq < state.tail_seq; ++seq) {
        if (seq != state.head_seq && seq % FREE_LIST_CAP == 0) {
            node = ConstLNode(pages.read_page(node)).next();
            held.insert(node);
        }
        held.insert(ConstLNode(pages.read_page(node)).get_ptr(seq % FREE_LIST_CAP));
    }
    for (const auto& [ptr, version] : freed_at) {
        EXPECT_TRUE(held.count(ptr) == 1) << "page " << ptr << " freed at version " << version << " was lost";
    }
}
