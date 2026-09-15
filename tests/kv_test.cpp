#include "storage/kv.h"

#include <sys/resource.h>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {
    std::vector<uint8_t> bytes(const std::string& s) {
        return std::vector<uint8_t>(s.begin(), s.end());
    }

    std::vector<uint8_t> key(int i) {
        return bytes("key" + std::to_string(i));
    }

    std::vector<uint8_t> value(int i) {
        return bytes("value" + std::to_string(i) + std::string(100, 'x'));
    }

    // The meta page's 8-byte fields, in the order kv.cpp writes them after the
    // 16-byte signature.
    enum MetaField : size_t {
        kMetaRoot,
        kMetaFlushed,
        kMetaHeadPage,
        kMetaHeadSeq,
        kMetaTailPage,
        kMetaTailSeq,
    };

    std::streamoff meta_offset(MetaField field) {
        return static_cast<std::streamoff>(16 + static_cast<size_t>(field) * 8);
    }

    uint64_t read_meta_field(const std::string& path, MetaField field) {
        std::ifstream f(path, std::ios::binary);
        f.seekg(meta_offset(field));
        uint64_t val = 0;
        f.read(reinterpret_cast<char*>(&val), sizeof(val));
        return val;
    }

    uintmax_t file_pages(const std::string& path) {
        return std::filesystem::file_size(path) / BTREE_PAGE_SIZE;
    }

    // Simulates a write error (e.g. "no space left") by capping the file
    // size via RLIMIT_FSIZE: with SIGXFSZ ignored, a write that would grow
    // the file past `limit` fails with EFBIG instead of killing the process.
    // A limit below the file's size also fails writes into the pages past it.
    class ScopedFileSizeLimit {
    public:
        explicit ScopedFileSizeLimit(rlim_t limit) {
            ::getrlimit(RLIMIT_FSIZE, &old_limit_);
            struct rlimit lim = old_limit_;
            lim.rlim_cur = limit;
            ::setrlimit(RLIMIT_FSIZE, &lim);
            old_handler_ = ::signal(SIGXFSZ, SIG_IGN);
        }
        ~ScopedFileSizeLimit() {
            ::setrlimit(RLIMIT_FSIZE, &old_limit_);
            ::signal(SIGXFSZ, old_handler_);
        }
        ScopedFileSizeLimit(const ScopedFileSizeLimit&) = delete;
        ScopedFileSizeLimit& operator=(const ScopedFileSizeLimit&) = delete;

    private:
        struct rlimit old_limit_;
        void (*old_handler_)(int);
    };

    class KVTest : public ::testing::Test {
    protected:
        void SetUp() override {
            const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
            path_ = (std::filesystem::temp_directory_path() /
                     (std::string("kv_test_") + info->test_suite_name() + "_" + info->name() + ".db"))
                        .string();
            std::filesystem::remove(path_);
        }

        void TearDown() override {
            std::filesystem::remove(path_);
        }

        std::string path_;
    };
}

TEST_F(KVTest, WhenKeyMissingThenGetReturnsNullopt) {
    KV db(path_);
    db.open();
    EXPECT_EQ(db.get(bytes("missing")), std::nullopt);
}

TEST_F(KVTest, OpenCreatesTheFileIfItDoesNotExist) {
    ASSERT_FALSE(std::filesystem::exists(path_));

    KV db(path_);
    db.open();

    EXPECT_TRUE(std::filesystem::exists(path_));
}

TEST_F(KVTest, OpenSucceedsOnAnAlreadyExistingFile) {
    uintmax_t size_before;
    {
        KV db(path_);
        db.open();
        db.set(bytes("key"), bytes("value"));
        db.close();
        size_before = std::filesystem::file_size(path_);
    }

    KV db(path_);
    EXPECT_NO_THROW(db.open());
    // The existing file must not be truncated by opening it.
    EXPECT_EQ(std::filesystem::file_size(path_), size_before);
}

TEST_F(KVTest, OpenThrowsWhenParentDirectoryDoesNotExist) {
    KV db((std::filesystem::path(path_).parent_path() / "no_such_dir" / "kv.db").string());
    EXPECT_THROW(db.open(), std::runtime_error);
}

TEST_F(KVTest, SetThenGetReturnsTheValue) {
    KV db(path_);
    db.open();
    db.set(bytes("key"), bytes("value"));
    EXPECT_EQ(db.get(bytes("key")), bytes("value"));
}

TEST_F(KVTest, SetThenDelRemovesTheKey) {
    KV db(path_);
    db.open();
    db.set(bytes("key"), bytes("value"));
    EXPECT_TRUE(db.del(bytes("key")));
    EXPECT_EQ(db.get(bytes("key")), std::nullopt);
}

TEST_F(KVTest, DelOnMissingKeyReturnsFalse) {
    KV db(path_);
    db.open();
    EXPECT_FALSE(db.del(bytes("missing")));
}

// ============================================================================
// InsertReq / DeleteReq
// ============================================================================
TEST_F(KVTest, WhenUpdateInsertsANewKeyThenAddedIsTrueAndOldIsEmpty) {
    KV db(path_);
    db.open();
    InsertReq req{bytes("key"), bytes("value")};
    req.old = bytes("stale");

    EXPECT_TRUE(db.update(&req));
    EXPECT_TRUE(req.added);
    EXPECT_TRUE(req.old.empty());
    EXPECT_EQ(db.get(bytes("key")), bytes("value"));
}

TEST_F(KVTest, WhenUpdateOverwritesAKeyThenOldHoldsThePreviousValue) {
    KV db(path_);
    db.open();
    db.set(bytes("key"), bytes("v1"));
    InsertReq req{bytes("key"), bytes("v2")};

    EXPECT_TRUE(db.update(&req));
    EXPECT_FALSE(req.added);
    EXPECT_EQ(req.old, bytes("v1"));
    EXPECT_EQ(db.get(bytes("key")), bytes("v2"));
}

TEST_F(KVTest, WhenTheUpdateModeIsNotMetThenUpdateReturnsFalseWithoutWriting) {
    KV db(path_);
    db.open();
    db.set(bytes("key"), bytes("v1"));
    uint64_t root = read_meta_field(path_, kMetaRoot);

    InsertReq insert{bytes("key"), bytes("v2"), UpdateMode::INSERT_ONLY};
    EXPECT_FALSE(db.update(&insert));
    EXPECT_EQ(insert.old, bytes("v1"));
    InsertReq update{bytes("missing"), bytes("v2"), UpdateMode::UPDATE_ONLY};
    EXPECT_FALSE(db.update(&update));

    EXPECT_EQ(db.get(bytes("key")), bytes("v1"));
    EXPECT_EQ(db.get(bytes("missing")), std::nullopt);
    EXPECT_EQ(read_meta_field(path_, kMetaRoot), root);
}

TEST_F(KVTest, WhenDelReqRemovesAKeyThenOldHoldsTheRemovedValue) {
    KV db(path_);
    db.open();
    db.set(bytes("key"), bytes("value"));
    DeleteReq req{bytes("key")};

    EXPECT_TRUE(db.del(&req));
    EXPECT_EQ(req.old, bytes("value"));
    EXPECT_EQ(db.get(bytes("key")), std::nullopt);

    DeleteReq missing{bytes("key")};
    EXPECT_FALSE(db.del(&missing));
    EXPECT_TRUE(missing.old.empty());
}

TEST_F(KVTest, DataSurvivesCloseAndReopen) {
    {
        KV db(path_);
        db.open();
        db.set(bytes("a"), bytes("1"));
        db.set(bytes("b"), bytes("2"));
        db.close();
    }

    KV db(path_);
    db.open();
    EXPECT_EQ(db.get(bytes("a")), bytes("1"));
    EXPECT_EQ(db.get(bytes("b")), bytes("2"));
}

TEST_F(KVTest, ManyKeysSurviveCloseAndReopen) {
    constexpr int kCount = 200;
    {
        KV db(path_);
        db.open();
        for (int i = 0; i < kCount; ++i) {
            db.set(bytes("key" + std::to_string(i)), bytes("value" + std::to_string(i)));
        }
        db.close();
    }

    KV db(path_);
    db.open();
    for (int i = 0; i < kCount; ++i) {
        EXPECT_EQ(db.get(bytes("key" + std::to_string(i))), bytes("value" + std::to_string(i)));
    }
}

TEST_F(KVTest, DeletesSurviveCloseAndReopen) {
    {
        KV db(path_);
        db.open();
        db.set(bytes("a"), bytes("1"));
        db.set(bytes("b"), bytes("2"));
        db.del(bytes("a"));
        db.close();
    }

    KV db(path_);
    db.open();
    EXPECT_EQ(db.get(bytes("a")), std::nullopt);
    EXPECT_EQ(db.get(bytes("b")), bytes("2"));
}

TEST_F(KVTest, ReopeningAnEmptyTreeStillFindsNothing) {
    {
        KV db(path_);
        db.open();
        db.close();
    }

    KV db(path_);
    db.open();
    EXPECT_EQ(db.get(bytes("anything")), std::nullopt);
}

TEST_F(KVTest, OpenThrowsOnBadSignature) {
    {
        std::ofstream f(path_, std::ios::binary);
        std::string garbage(BTREE_PAGE_SIZE, '\xff');
        f.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
    }

    KV db(path_);
    EXPECT_THROW(db.open(), std::runtime_error);
}

TEST_F(KVTest, OpenThrowsWhenFileIsTooSmallForAMetaPage) {
    {
        std::ofstream f(path_, std::ios::binary);
        f << "DB"; // shorter than the meta page's fields
    }

    KV db(path_);
    EXPECT_THROW(db.open(), std::runtime_error);
}

TEST_F(KVTest, SetThrowsWhenTheWriteFails) {
    KV db(path_);
    db.open();
    db.set(bytes("a"), bytes("1"));

    ScopedFileSizeLimit limit(std::filesystem::file_size(path_));
    EXPECT_THROW(db.set(bytes("b"), bytes("2")), std::runtime_error);
}

TEST_F(KVTest, ReadAfterAFailedSetBehavesAsIfNothingHappened) {
    KV db(path_);
    db.open();
    db.set(bytes("a"), bytes("1"));

    {
        ScopedFileSizeLimit limit(std::filesystem::file_size(path_));
        EXPECT_THROW(db.set(bytes("b"), bytes("2")), std::runtime_error);
    }

    EXPECT_EQ(db.get(bytes("a")), bytes("1"));
    EXPECT_EQ(db.get(bytes("b")), std::nullopt);
}

TEST_F(KVTest, ReadAfterAFailedDelBehavesAsIfNothingHappened) {
    KV db(path_);
    db.open();
    db.set(bytes("a"), bytes("1"));
    db.set(bytes("b"), bytes("2"));

    {
        // A delete reuses a page rather than appending, so the file size has
        // to be capped below the tree's pages for its write to fail.
        ScopedFileSizeLimit limit(2 * BTREE_PAGE_SIZE);
        EXPECT_THROW(db.del(bytes("a")), std::runtime_error);
    }

    EXPECT_EQ(db.get(bytes("a")), bytes("1"));
    EXPECT_EQ(db.get(bytes("b")), bytes("2"));
}

TEST_F(KVTest, UpdateFailsAgainWhileTheErrorPersists) {
    KV db(path_);
    db.open();
    db.set(bytes("a"), bytes("1"));

    ScopedFileSizeLimit limit(std::filesystem::file_size(path_));
    EXPECT_THROW(db.set(bytes("b"), bytes("2")), std::runtime_error);
    EXPECT_THROW(db.set(bytes("c"), bytes("3")), std::runtime_error);

    EXPECT_EQ(db.get(bytes("a")), bytes("1"));
    EXPECT_EQ(db.get(bytes("b")), std::nullopt);
    EXPECT_EQ(db.get(bytes("c")), std::nullopt);
}

TEST_F(KVTest, RecoversOnceATemporaryWriteErrorIsResolved) {
    KV db(path_);
    db.open();
    db.set(bytes("a"), bytes("1"));

    {
        ScopedFileSizeLimit limit(std::filesystem::file_size(path_));
        EXPECT_THROW(db.set(bytes("b"), bytes("2")), std::runtime_error);
    }

    // The limit is lifted: the next update recovers, first repairing the
    // on-disk meta page left in an unknown state by the earlier failure.
    EXPECT_NO_THROW(db.set(bytes("b"), bytes("2")));
    EXPECT_EQ(db.get(bytes("a")), bytes("1"));
    EXPECT_EQ(db.get(bytes("b")), bytes("2"));
}

// ============================================================================
// Page reuse
// ============================================================================
TEST_F(KVTest, WhenUpdatesRunThenTheMetaPageRecordsTheFreeListPosition) {
    KV db(path_);
    db.open();
    db.set(bytes("a"), bytes("1"));

    // The list starts as 1 empty node, on the page reserved at creation.
    EXPECT_EQ(read_meta_field(path_, kMetaHeadPage), 1u);
    EXPECT_EQ(read_meta_field(path_, kMetaTailPage), 1u);
    EXPECT_EQ(read_meta_field(path_, kMetaHeadSeq), 0u);
    EXPECT_EQ(read_meta_field(path_, kMetaTailSeq), 0u);

    db.set(bytes("a"), bytes("2")); // replaces the root, freeing the old one
    EXPECT_EQ(read_meta_field(path_, kMetaTailSeq), 1u);
    EXPECT_EQ(read_meta_field(path_, kMetaHeadSeq), 0u);

    db.set(bytes("a"), bytes("3")); // takes that page back
    EXPECT_EQ(read_meta_field(path_, kMetaHeadSeq), 1u);
    EXPECT_EQ(read_meta_field(path_, kMetaTailSeq), 2u);
}

TEST_F(KVTest, WhenTheMetaPageHasNoFreeListNodeThenOpenThrows) {
    {
        KV db(path_);
        db.open();
        db.set(bytes("a"), bytes("1"));
        db.close();
    }

    // Zero the free list fields; no committed db has a list without a node.
    std::fstream f(path_, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(meta_offset(kMetaHeadPage));
    std::string zeros(4 * 8, '\0');
    f.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    f.close();

    KV db(path_);
    EXPECT_THROW(db.open(), std::runtime_error);
}

TEST_F(KVTest, WhenAKeyIsOverwrittenManyTimesThenTheFileStopsGrowing) {
    constexpr int kWarmup = 20;
    constexpr int kCount = 2000;

    KV db(path_);
    db.open();
    for (int i = 0; i < kWarmup; ++i) {
        db.set(bytes("key"), value(i));
    }
    uintmax_t warm_pages = file_pages(path_);

    for (int i = kWarmup; i < kCount; ++i) {
        db.set(bytes("key"), value(i));
    }

    // Every update replaces the root page and hands the old one back, so the
    // file only grows for the occasional new free list node.
    EXPECT_LE(file_pages(path_), warm_pages + 1);
    EXPECT_EQ(db.get(bytes("key")), value(kCount - 1));
}

TEST_F(KVTest, WhenKeysAreReinsertedAfterDeletesThenTheFileDoesNotGrow) {
    constexpr int kCount = 300;

    KV db(path_);
    db.open();
    uintmax_t baseline = 0;
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < kCount; ++i) {
            db.set(key(i), value(i));
        }
        for (int i = 0; i < kCount; ++i) {
            ASSERT_TRUE(db.del(key(i))) << "round " << round << ", key " << i;
        }
        if (round == 0) {
            baseline = file_pages(path_); // the 1st round is what sizes the file
        } else {
            EXPECT_EQ(file_pages(path_), baseline) << "round " << round;
        }
    }
    EXPECT_EQ(db.get(key(0)), std::nullopt);
}

TEST_F(KVTest, WhenDeletedPagesAreReusedThenTheDeletedKeysStayGone) {
    constexpr int kCount = 200;

    KV db(path_);
    db.open();
    for (int i = 0; i < kCount; ++i) {
        db.set(key(i), value(i));
    }
    for (int i = 0; i < kCount; i += 2) {
        ASSERT_TRUE(db.del(key(i)));
    }
    // Enough churn that the deleted keys' pages are handed out again.
    for (int i = kCount; i < 3 * kCount; ++i) {
        db.set(key(i), value(i));
    }

    for (int i = 0; i < kCount; i += 2) {
        EXPECT_EQ(db.get(key(i)), std::nullopt) << "key " << i;
    }
    for (int i = 1; i < kCount; i += 2) {
        EXPECT_EQ(db.get(key(i)), value(i)) << "key " << i;
    }
    for (int i = kCount; i < 3 * kCount; ++i) {
        EXPECT_EQ(db.get(key(i)), value(i)) << "key " << i;
    }
}

TEST_F(KVTest, WhenReopenedThenPagesFreedBeforeTheCloseAreStillReused) {
    constexpr int kCount = 100;
    {
        KV db(path_);
        db.open();
        for (int i = 0; i < kCount; ++i) {
            db.set(key(i), value(i));
        }
        for (int i = 0; i < kCount; ++i) {
            db.del(key(i));
        }
        db.close();
    }

    KV db(path_);
    db.open();
    uintmax_t pages_at_open = file_pages(path_);

    // The list restored from the meta page still holds the pages freed before
    // the reopen, so refilling the tree needs at most 1 new page (for a list
    // node) instead of the ~10 the first fill took.
    for (int i = 0; i < kCount; ++i) {
        db.set(key(i), value(i));
    }

    EXPECT_LE(file_pages(path_), pages_at_open + 1);
    for (int i = 0; i < kCount; ++i) {
        EXPECT_EQ(db.get(key(i)), value(i)) << "key " << i;
    }
}

TEST_F(KVTest, WhenAnUpdateFailsAfterRewritingAFreeListNodeThenDataSurvives) {
    constexpr int kCount = 60;

    KV db(path_);
    db.open();
    for (int i = 0; i < kCount; ++i) {
        db.set(key(i), value(i));
    }
    for (int i = 0; i < kCount / 2; ++i) {
        ASSERT_TRUE(db.del(key(i)));
    }

    {
        // The free list node on page 1 can still be written, but the tree
        // pages past it cannot: the update fails midway through.
        ScopedFileSizeLimit limit(2 * BTREE_PAGE_SIZE);
        EXPECT_THROW(db.set(bytes("zzz"), bytes("1")), std::runtime_error);
    }

    EXPECT_EQ(db.get(bytes("zzz")), std::nullopt);
    for (int i = kCount / 2; i < kCount; ++i) {
        EXPECT_EQ(db.get(key(i)), value(i)) << "key " << i;
    }

    db.set(bytes("zzz"), bytes("1")); // recovers
    db.close();

    KV reopened(path_);
    reopened.open();
    EXPECT_EQ(reopened.get(bytes("zzz")), bytes("1"));
    for (int i = 0; i < kCount / 2; ++i) {
        EXPECT_EQ(reopened.get(key(i)), std::nullopt) << "key " << i;
    }
    for (int i = kCount / 2; i < kCount; ++i) {
        EXPECT_EQ(reopened.get(key(i)), value(i)) << "key " << i;
    }
}

// A long random mix of writes and deletes, compared against a std::map, with
// reopens in between: page reuse must never hand a live page to a new node.
TEST_F(KVTest, WhenARandomWorkloadRunsAcrossReopensThenItMatchesAReferenceMap) {
    std::map<std::vector<uint8_t>, std::vector<uint8_t>> model;
    std::mt19937 rng(20250911);

    for (int session = 0; session < 4; ++session) {
        KV db(path_);
        db.open();
        for (int op = 0; op < 500; ++op) {
            int k = static_cast<int>(rng() % 150);
            if (rng() % 3 == 0) {
                bool deleted = db.del(key(k));
                EXPECT_EQ(deleted, model.erase(key(k)) == 1) << "key " << k;
            } else {
                int v = static_cast<int>(rng() % 1000);
                db.set(key(k), value(v));
                model[key(k)] = value(v);
            }
        }
        for (const auto& [k, v] : model) {
            ASSERT_EQ(db.get(k), v) << "session " << session;
        }
        db.close();
    }

    KV db(path_);
    db.open();
    for (const auto& [k, v] : model) {
        EXPECT_EQ(db.get(k), v);
    }
    for (int k = 0; k < 150; ++k) {
        if (!model.count(key(k))) {
            EXPECT_EQ(db.get(key(k)), std::nullopt) << "key " << k;
        }
    }
}

TEST_F(KVTest, RecoveredDataSurvivesCloseAndReopen) {
    {
        KV db(path_);
        db.open();
        db.set(bytes("a"), bytes("1"));

        {
            ScopedFileSizeLimit limit(std::filesystem::file_size(path_));
            EXPECT_THROW(db.set(bytes("b"), bytes("2")), std::runtime_error);
        }

        db.set(bytes("b"), bytes("2"));
        db.close();
    }

    KV db(path_);
    db.open();
    EXPECT_EQ(db.get(bytes("a")), bytes("1"));
    EXPECT_EQ(db.get(bytes("b")), bytes("2"));
}
