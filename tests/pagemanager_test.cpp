#include "storage/pagemanager.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {
    std::vector<uint8_t> bytes(const std::string& s) {
        return std::vector<uint8_t>(s.begin(), s.end());
    }

    BNode make_leaf(const std::vector<uint8_t>& key, const std::vector<uint8_t>& val) {
        BNode node(BTREE_PAGE_SIZE);
        node.set_header(BNODE_LEAF, 1);
        node.node_append_kv(0, 0, key, val);
        return node;
    }

    // Page 0 is the meta page and page 1 the free list's first node, so the
    // pages a fresh file hands out start here.
    constexpr uint64_t kFirstDataPage = 2;

    class PageManagerTest : public ::testing::Test {
    protected:
        void SetUp() override {
            const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
            path_ = (std::filesystem::temp_directory_path() /
                     (std::string("pagemanager_test_") + info->name() + ".db"))
                        .string();
            std::filesystem::remove(path_);
            fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
            ASSERT_GE(fd_, 0);
        }

        void TearDown() override {
            ::close(fd_);
            std::filesystem::remove(path_);
        }

        std::string path_;
        int fd_ = -1;
    };
}

TEST_F(PageManagerTest, WhenPagesAreCreatedThenPointersFollowTheReservedPages) {
    PageManager pages(fd_);
    uint64_t p0 = pages.new_page(make_leaf(bytes("k0"), bytes("v0")));
    uint64_t p1 = pages.new_page(make_leaf(bytes("k1"), bytes("v1")));

    EXPECT_EQ(p0, kFirstDataPage);
    EXPECT_EQ(p1, kFirstDataPage + 1);
}

TEST_F(PageManagerTest, GetReturnsThePageWrittenAfterWritePages) {
    PageManager pages(fd_);
    uint64_t ptr = pages.new_page(make_leaf(bytes("key"), bytes("value")));
    pages.write_pages();

    BNode node = pages.get(ptr);
    EXPECT_EQ(node.get_key(0), bytes("key"));
    EXPECT_EQ(node.get_val(0), bytes("value"));
}

TEST_F(PageManagerTest, WhenAPageIsNotWrittenYetThenGetReturnsTheBufferedCopy) {
    PageManager pages(fd_);
    uint64_t ptr = pages.new_page(make_leaf(bytes("key"), bytes("value")));

    EXPECT_EQ(pages.get(ptr).get_val(0), bytes("value"));
    EXPECT_EQ(std::filesystem::file_size(path_), 0);
}

TEST_F(PageManagerTest, WritePagesAppendsPagesAfterTheReservedPages) {
    PageManager pages(fd_);
    pages.new_page(make_leaf(bytes("a"), bytes("1")));
    pages.new_page(make_leaf(bytes("b"), bytes("2")));
    pages.write_pages();

    EXPECT_EQ(std::filesystem::file_size(path_), 4 * BTREE_PAGE_SIZE);
    EXPECT_EQ(pages.flushed_pages(), 4u);
}

TEST_F(PageManagerTest, MultipleWritePagesCallsAppendSequentially) {
    PageManager pages(fd_);
    uint64_t p0 = pages.new_page(make_leaf(bytes("a"), bytes("1")));
    pages.write_pages();
    uint64_t p1 = pages.new_page(make_leaf(bytes("b"), bytes("2")));
    pages.write_pages();

    EXPECT_EQ(pages.get(p0).get_key(0), bytes("a"));
    EXPECT_EQ(pages.get(p1).get_key(0), bytes("b"));
}

TEST_F(PageManagerTest, GetThrowsForAPointerThatWasNeverWritten) {
    PageManager pages(fd_);
    EXPECT_THROW(pages.get(kFirstDataPage), std::out_of_range);
}

TEST_F(PageManagerTest, WritePagesWithNothingBufferedIsANoOp) {
    PageManager pages(fd_);
    pages.write_pages(); // lays down the empty free list node
    pages.write_pages();

    EXPECT_EQ(pages.flushed_pages(), 2u);
    EXPECT_EQ(std::filesystem::file_size(path_), 2 * BTREE_PAGE_SIZE);
}

TEST_F(PageManagerTest, ReopeningWithAFlushedPagesCountMakesExistingPagesReadable) {
    uint64_t ptr;
    FreeListState free_state;
    uint64_t flushed;
    {
        PageManager pages(fd_);
        ptr = pages.new_page(make_leaf(bytes("key"), bytes("value")));
        pages.write_pages();
        free_state = pages.free_state();
        flushed = pages.flushed_pages();
    }

    // A PageManager seeded with the state from the meta page must see pages a
    // prior instance wrote, without needing a write_pages() call.
    PageManager reopened(fd_, flushed, free_state);
    BNode node = reopened.get(ptr);
    EXPECT_EQ(node.get_key(0), bytes("key"));
    EXPECT_EQ(node.get_val(0), bytes("value"));
}

TEST_F(PageManagerTest, ReopenedPageManagerContinuesAllocatingAfterFlushedPages) {
    {
        PageManager pages(fd_);
        pages.new_page(make_leaf(bytes("a"), bytes("1")));
        pages.write_pages();
    }

    PageManager reopened(fd_, /*flushed_pages=*/3, {1, 0, 1, 0});
    uint64_t ptr = reopened.new_page(make_leaf(bytes("b"), bytes("2")));
    EXPECT_EQ(ptr, 3u);
}

TEST_F(PageManagerTest, RevertDiscardsBufferedPagesAndResetsFlushedCount) {
    PageManager pages(fd_);
    pages.new_page(make_leaf(bytes("a"), bytes("1")));
    pages.write_pages();
    pages.new_page(make_leaf(bytes("b"), bytes("2"))); // buffered, not yet written

    pages.revert(3, pages.free_state());

    EXPECT_EQ(pages.flushed_pages(), 3u);
    // The buffered page was discarded, so allocation resumes at 3.
    uint64_t ptr = pages.new_page(make_leaf(bytes("c"), bytes("3")));
    EXPECT_EQ(ptr, 3u);
}

// ============================================================================
// Page reuse
// ============================================================================
TEST_F(PageManagerTest, WhenAFreedPageIsReleasedThenNewPageReusesIt) {
    PageManager pages(fd_);
    uint64_t freed = pages.new_page(make_leaf(bytes("a"), bytes("1")));
    pages.write_pages();

    pages.del(freed);
    pages.write_pages();
    pages.release_freed_pages();

    EXPECT_EQ(pages.new_page(make_leaf(bytes("b"), bytes("2"))), freed);
}

TEST_F(PageManagerTest, WhenAFreedPageIsNotReleasedThenNewPageAppendsInstead) {
    PageManager pages(fd_);
    uint64_t freed = pages.new_page(make_leaf(bytes("a"), bytes("1")));
    pages.write_pages();

    // The page still belongs to the version on disk until this update commits.
    pages.del(freed);
    EXPECT_NE(pages.new_page(make_leaf(bytes("b"), bytes("2"))), freed);
}

TEST_F(PageManagerTest, WhenPagesAreReusedThenTheFileStopsGrowing) {
    PageManager pages(fd_);
    uint64_t ptr = pages.new_page(make_leaf(bytes("a"), bytes("1")));
    pages.write_pages();
    pages.release_freed_pages();

    uintmax_t size_after_warmup = 0;
    for (int i = 0; i < 10; ++i) {
        pages.del(ptr);
        ptr = pages.new_page(make_leaf(bytes("k"), bytes("v" + std::to_string(i))));
        pages.write_pages();
        pages.release_freed_pages();
        if (i == 0) {
            size_after_warmup = std::filesystem::file_size(path_); // the 1st update still appends
        }
    }

    EXPECT_EQ(std::filesystem::file_size(path_), size_after_warmup);
    EXPECT_EQ(pages.flushed_pages(), 4u);
    EXPECT_EQ(pages.get(ptr).get_val(0), bytes("v9"));
}

TEST_F(PageManagerTest, WhenAReusedPageIsWrittenThenItsNewContentIsOnDisk) {
    PageManager pages(fd_);
    uint64_t ptr = pages.new_page(make_leaf(bytes("old"), bytes("1")));
    pages.write_pages();
    pages.release_freed_pages();

    pages.del(ptr);
    pages.release_freed_pages();
    ASSERT_EQ(pages.new_page(make_leaf(bytes("new"), bytes("2"))), ptr);
    pages.write_pages();
    uint64_t flushed = pages.flushed_pages();
    FreeListState free_state = pages.free_state();

    PageManager reopened(fd_, flushed, free_state);
    EXPECT_EQ(reopened.get(ptr).get_key(0), bytes("new"));
}

TEST_F(PageManagerTest, WhenAPageIsDeletedThenTheFreeListTailAdvances) {
    PageManager pages(fd_);
    uint64_t ptr = pages.new_page(make_leaf(bytes("a"), bytes("1")));
    pages.write_pages();
    ASSERT_EQ(pages.free_state().tail_seq, 0u);

    pages.del(ptr);

    EXPECT_EQ(pages.free_state().tail_seq, 1u);
    EXPECT_EQ(pages.free_state().head_seq, 0u);
    EXPECT_EQ(pages.free_state().head_page, 1u); // the list node reserved at creation
}

TEST_F(PageManagerTest, WhenAnUpdateIsRevertedThenItsFreedPageIsLiveAgain) {
    PageManager pages(fd_);
    uint64_t ptr = pages.new_page(make_leaf(bytes("a"), bytes("1")));
    pages.write_pages();
    pages.release_freed_pages();
    uint64_t flushed = pages.flushed_pages();
    FreeListState committed = pages.free_state();

    // A failed update: it freed a page, then appended a replacement.
    pages.del(ptr);
    pages.new_page(make_leaf(bytes("b"), bytes("2")));
    pages.revert(flushed, committed);

    EXPECT_EQ(pages.free_state().tail_seq, committed.tail_seq);
    EXPECT_EQ(pages.flushed_pages(), flushed);
    // The page is live again, so the next update must not be handed it.
    EXPECT_NE(pages.new_page(make_leaf(bytes("c"), bytes("3"))), ptr);
}

TEST_F(PageManagerTest, WhenReopenedThenPagesFreedBeforeAreStillReused) {
    uint64_t freed;
    uint64_t flushed;
    FreeListState free_state;
    {
        PageManager pages(fd_);
        freed = pages.new_page(make_leaf(bytes("a"), bytes("1")));
        pages.new_page(make_leaf(bytes("b"), bytes("2")));
        pages.write_pages();
        pages.release_freed_pages();

        pages.del(freed);
        pages.write_pages();
        flushed = pages.flushed_pages();
        free_state = pages.free_state();
    }

    PageManager reopened(fd_, flushed, free_state);
    EXPECT_EQ(reopened.new_page(make_leaf(bytes("c"), bytes("3"))), freed);
}

TEST_F(PageManagerTest, WhenMorePagesAreFreedThanOneNodeHoldsThenNoneIsLost) {
    PageManager pages(fd_);
    // Enough pages to fill the first list node and spill into a second one.
    std::vector<uint64_t> ptrs;
    for (size_t i = 0; i < FREE_LIST_CAP + 10; ++i) {
        ptrs.push_back(pages.new_page(make_leaf(bytes("k"), bytes("v"))));
    }
    pages.write_pages();
    pages.release_freed_pages();

    for (uint64_t ptr : ptrs) {
        pages.del(ptr);
    }
    pages.write_pages();
    pages.release_freed_pages();
    uint64_t flushed_before = pages.flushed_pages();

    // Every freed page comes back before the file has to grow again.
    std::vector<uint64_t> reused;
    for (size_t i = 0; i < ptrs.size(); ++i) {
        reused.push_back(pages.new_page(make_leaf(bytes("k"), bytes("v"))));
    }
    EXPECT_EQ(pages.flushed_pages(), flushed_before);

    std::sort(ptrs.begin(), ptrs.end());
    std::sort(reused.begin(), reused.end());
    EXPECT_EQ(reused, ptrs);
}
