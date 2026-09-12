#include "pagemanager.h"

#include <fcntl.h>
#include <unistd.h>

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

TEST_F(PageManagerTest, NewPageAssignsSequentialPointersStartingAtOne) {
    PageManager pages(fd_);
    uint64_t p0 = pages.new_page(make_leaf(bytes("k0"), bytes("v0")));
    uint64_t p1 = pages.new_page(make_leaf(bytes("k1"), bytes("v1")));

    EXPECT_EQ(p0, 1u);
    EXPECT_EQ(p1, 2u);
}

TEST_F(PageManagerTest, GetReturnsThePageWrittenAfterWritePages) {
    PageManager pages(fd_);
    uint64_t ptr = pages.new_page(make_leaf(bytes("key"), bytes("value")));
    pages.write_pages();

    BNode node = pages.get(ptr);
    EXPECT_EQ(node.get_key(0), bytes("key"));
    EXPECT_EQ(node.get_val(0), bytes("value"));
}

TEST_F(PageManagerTest, WritePagesAppendsPagesAfterTheReservedMetaPage) {
    PageManager pages(fd_);
    pages.new_page(make_leaf(bytes("a"), bytes("1")));
    pages.new_page(make_leaf(bytes("b"), bytes("2")));
    pages.write_pages();

    EXPECT_EQ(std::filesystem::file_size(path_), 3 * BTREE_PAGE_SIZE);
    EXPECT_EQ(pages.flushed_pages(), 3u);
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
    EXPECT_THROW(pages.get(1), std::out_of_range);
}

TEST_F(PageManagerTest, DelDoesNotAffectOtherPages) {
    PageManager pages(fd_);
    uint64_t p0 = pages.new_page(make_leaf(bytes("a"), bytes("1")));
    uint64_t p1 = pages.new_page(make_leaf(bytes("b"), bytes("2")));
    pages.write_pages();

    pages.del(p0);
    EXPECT_EQ(pages.get(p1).get_key(0), bytes("b"));
}

TEST_F(PageManagerTest, WritePagesWithNoPendingPagesIsANoOp) {
    PageManager pages(fd_);
    pages.write_pages();

    EXPECT_EQ(pages.flushed_pages(), 1u);
    EXPECT_EQ(std::filesystem::file_size(path_), 0);
}

TEST_F(PageManagerTest, ReopeningWithAFlushedPagesCountMakesExistingPagesReadable) {
    uint64_t ptr;
    {
        PageManager pages(fd_);
        ptr = pages.new_page(make_leaf(bytes("key"), bytes("value")));
        pages.write_pages();
    }

    // Simulate reopening the file: a fresh PageManager seeded with the
    // flushed-page count restored from the meta page must see pages
    // written by a prior instance without needing a write_pages() call.
    PageManager reopened(fd_, /*flushed_pages=*/2);
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

    PageManager reopened(fd_, /*flushed_pages=*/2);
    uint64_t ptr = reopened.new_page(make_leaf(bytes("b"), bytes("2")));
    EXPECT_EQ(ptr, 2u);
}

TEST_F(PageManagerTest, RevertDiscardsBufferedPagesAndResetsFlushedCount) {
    PageManager pages(fd_);
    pages.new_page(make_leaf(bytes("a"), bytes("1")));
    pages.write_pages();
    pages.new_page(make_leaf(bytes("b"), bytes("2"))); // buffered, not yet written

    pages.revert(2);

    EXPECT_EQ(pages.flushed_pages(), 2u);
    // The buffered page was discarded, so allocation resumes at 2.
    uint64_t ptr = pages.new_page(make_leaf(bytes("c"), bytes("3")));
    EXPECT_EQ(ptr, 2u);
}
