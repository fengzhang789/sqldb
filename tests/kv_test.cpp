#include "storage/kv.h"

#include <sys/resource.h>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
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
        kMetaVersion,
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

    std::string file_contents(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }

    // The live tree root, through the pointer every BIter keeps to its tree.
    uint64_t live_root(const KVTX& tx) {
        return tx.seek(bytes("key"), CMP_GE).tree->root;
    }

    // The pre-transaction KV API, for the tests of per-update behavior: each call runs in its own transaction.
    struct AutoCommitKV : KV {
        using KV::KV;

        std::optional<std::vector<uint8_t>> get(const std::vector<uint8_t>& key) {
            KVTX tx;
            begin(&tx);
            std::optional<std::vector<uint8_t>> val = tx.get(key);
            commit(&tx);
            return val;
        }

        void set(const std::vector<uint8_t>& key, const std::vector<uint8_t>& val) {
            KVTX tx;
            begin(&tx);
            tx.set(key, val);
            commit(&tx);
        }

        bool del(const std::vector<uint8_t>& key) {
            KVTX tx;
            begin(&tx);
            bool deleted = tx.del(key);
            commit(&tx);
            return deleted;
        }

        bool update(InsertReq* req) {
            KVTX tx;
            begin(&tx);
            bool updated = tx.update(req);
            commit(&tx);
            return updated;
        }

        bool del(DeleteReq* req) {
            KVTX tx;
            begin(&tx);
            bool deleted = tx.del(req);
            commit(&tx);
            return deleted;
        }
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
    AutoCommitKV db(path_);
    db.open();
    EXPECT_EQ(db.get(bytes("missing")), std::nullopt);
}

TEST_F(KVTest, OpenCreatesTheFileIfItDoesNotExist) {
    ASSERT_FALSE(std::filesystem::exists(path_));

    AutoCommitKV db(path_);
    db.open();

    EXPECT_TRUE(std::filesystem::exists(path_));
}

TEST_F(KVTest, OpenSucceedsOnAnAlreadyExistingFile) {
    uintmax_t size_before;
    {
        AutoCommitKV db(path_);
        db.open();
        db.set(bytes("key"), bytes("value"));
        db.close();
        size_before = std::filesystem::file_size(path_);
    }

    AutoCommitKV db(path_);
    EXPECT_NO_THROW(db.open());
    // The existing file must not be truncated by opening it.
    EXPECT_EQ(std::filesystem::file_size(path_), size_before);
}

TEST_F(KVTest, OpenThrowsWhenParentDirectoryDoesNotExist) {
    KV db((std::filesystem::path(path_).parent_path() / "no_such_dir" / "kv.db").string());
    EXPECT_THROW(db.open(), std::runtime_error);
}

TEST_F(KVTest, SetThenGetReturnsTheValue) {
    AutoCommitKV db(path_);
    db.open();
    db.set(bytes("key"), bytes("value"));
    EXPECT_EQ(db.get(bytes("key")), bytes("value"));
}

TEST_F(KVTest, SetThenDelRemovesTheKey) {
    AutoCommitKV db(path_);
    db.open();
    db.set(bytes("key"), bytes("value"));
    EXPECT_TRUE(db.del(bytes("key")));
    EXPECT_EQ(db.get(bytes("key")), std::nullopt);
}

TEST_F(KVTest, DelOnMissingKeyReturnsFalse) {
    AutoCommitKV db(path_);
    db.open();
    EXPECT_FALSE(db.del(bytes("missing")));
}

// ============================================================================
// InsertReq / DeleteReq
// ============================================================================
TEST_F(KVTest, WhenUpdateInsertsANewKeyThenAddedIsTrueAndOldIsEmpty) {
    AutoCommitKV db(path_);
    db.open();
    InsertReq req{bytes("key"), bytes("value")};
    req.old = bytes("stale");

    EXPECT_TRUE(db.update(&req));
    EXPECT_TRUE(req.added);
    EXPECT_TRUE(req.old.empty());
    EXPECT_EQ(db.get(bytes("key")), bytes("value"));
}

TEST_F(KVTest, WhenUpdateOverwritesAKeyThenOldHoldsThePreviousValue) {
    AutoCommitKV db(path_);
    db.open();
    db.set(bytes("key"), bytes("v1"));
    InsertReq req{bytes("key"), bytes("v2")};

    EXPECT_TRUE(db.update(&req));
    EXPECT_FALSE(req.added);
    EXPECT_EQ(req.old, bytes("v1"));
    EXPECT_EQ(db.get(bytes("key")), bytes("v2"));
}

TEST_F(KVTest, WhenTheUpdateModeIsNotMetThenUpdateReturnsFalseWithoutWriting) {
    AutoCommitKV db(path_);
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
    AutoCommitKV db(path_);
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
        AutoCommitKV db(path_);
        db.open();
        db.set(bytes("a"), bytes("1"));
        db.set(bytes("b"), bytes("2"));
        db.close();
    }

    AutoCommitKV db(path_);
    db.open();
    EXPECT_EQ(db.get(bytes("a")), bytes("1"));
    EXPECT_EQ(db.get(bytes("b")), bytes("2"));
}

TEST_F(KVTest, ManyKeysSurviveCloseAndReopen) {
    constexpr int kCount = 200;
    {
        AutoCommitKV db(path_);
        db.open();
        for (int i = 0; i < kCount; ++i) {
            db.set(bytes("key" + std::to_string(i)), bytes("value" + std::to_string(i)));
        }
        db.close();
    }

    AutoCommitKV db(path_);
    db.open();
    for (int i = 0; i < kCount; ++i) {
        EXPECT_EQ(db.get(bytes("key" + std::to_string(i))), bytes("value" + std::to_string(i)));
    }
}

TEST_F(KVTest, DeletesSurviveCloseAndReopen) {
    {
        AutoCommitKV db(path_);
        db.open();
        db.set(bytes("a"), bytes("1"));
        db.set(bytes("b"), bytes("2"));
        db.del(bytes("a"));
        db.close();
    }

    AutoCommitKV db(path_);
    db.open();
    EXPECT_EQ(db.get(bytes("a")), std::nullopt);
    EXPECT_EQ(db.get(bytes("b")), bytes("2"));
}

TEST_F(KVTest, ReopeningAnEmptyTreeStillFindsNothing) {
    {
        AutoCommitKV db(path_);
        db.open();
        db.close();
    }

    AutoCommitKV db(path_);
    db.open();
    EXPECT_EQ(db.get(bytes("anything")), std::nullopt);
}

TEST_F(KVTest, OpenThrowsOnBadSignature) {
    {
        std::ofstream f(path_, std::ios::binary);
        std::string garbage(BTREE_PAGE_SIZE, '\xff');
        f.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
    }

    AutoCommitKV db(path_);
    EXPECT_THROW(db.open(), std::runtime_error);
}

TEST_F(KVTest, OpenThrowsWhenFileIsTooSmallForAMetaPage) {
    {
        std::ofstream f(path_, std::ios::binary);
        f << "DB"; // shorter than the meta page's fields
    }

    AutoCommitKV db(path_);
    EXPECT_THROW(db.open(), std::runtime_error);
}

TEST_F(KVTest, SetThrowsWhenTheWriteFails) {
    AutoCommitKV db(path_);
    db.open();
    db.set(bytes("a"), bytes("1"));

    ScopedFileSizeLimit limit(std::filesystem::file_size(path_));
    EXPECT_THROW(db.set(bytes("b"), bytes("2")), std::runtime_error);
}

TEST_F(KVTest, ReadAfterAFailedSetBehavesAsIfNothingHappened) {
    AutoCommitKV db(path_);
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
    AutoCommitKV db(path_);
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
    AutoCommitKV db(path_);
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
    AutoCommitKV db(path_);
    db.open();
    db.set(bytes("a"), bytes("1"));

    {
        ScopedFileSizeLimit limit(std::filesystem::file_size(path_));
        EXPECT_THROW(db.set(bytes("b"), bytes("2")), std::runtime_error);
    }

    // The limit is lifted: the failed commit never reached the meta page and
    // was rolled back, so the next update commits on top of the last one.
    EXPECT_NO_THROW(db.set(bytes("b"), bytes("2")));
    EXPECT_EQ(db.get(bytes("a")), bytes("1"));
    EXPECT_EQ(db.get(bytes("b")), bytes("2"));
}

// ============================================================================
// Page reuse
// ============================================================================
TEST_F(KVTest, WhenUpdatesRunThenTheMetaPageRecordsTheFreeListPosition) {
    AutoCommitKV db(path_);
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

TEST_F(KVTest, WhenTransactionsCommitThenTheMetaPageVersionCountsTheOnesThatWroteAcrossReopens) {
    {
        KV db(path_);
        db.open();
        for (int i = 0; i < 3; ++i) {
            KVTX tx;
            db.begin(&tx);
            tx.set(key(i), value(i));
            db.commit(&tx);
        }
        EXPECT_EQ(read_meta_field(path_, kMetaVersion), 3u);

        KVTX aborted;
        db.begin(&aborted);
        aborted.set(key(9), value(9));
        db.abort(&aborted);
        KVTX read_only;
        db.begin(&read_only);
        EXPECT_EQ(read_only.get(key(0)), value(0));
        db.commit(&read_only);
        db.close();
    }

    KV db(path_);
    db.open();
    KVTX tx;
    db.begin(&tx);
    tx.set(key(3), value(3));
    db.commit(&tx);
    EXPECT_EQ(read_meta_field(path_, kMetaVersion), 4u); // continues from the version restored at open
}

TEST_F(KVTest, WhenTheMetaPageHasNoFreeListNodeThenOpenThrows) {
    {
        AutoCommitKV db(path_);
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

    AutoCommitKV db(path_);
    EXPECT_THROW(db.open(), std::runtime_error);
}

TEST_F(KVTest, WhenAKeyIsOverwrittenManyTimesThenTheFileStopsGrowing) {
    constexpr int kWarmup = 20;
    constexpr int kCount = 2000;

    AutoCommitKV db(path_);
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

TEST_F(KVTest, WhenKeysAreReinsertedAfterDeletesThenTheFileStopsGrowing) {
    constexpr int kCount = 300;

    AutoCommitKV db(path_);
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
            // A free list node can fill while nothing is reusable yet, so a later round may append 1 page for it.
            EXPECT_LE(file_pages(path_), baseline + 1) << "round " << round;
        }
    }
    EXPECT_EQ(db.get(key(0)), std::nullopt);
}

TEST_F(KVTest, WhenDeletedPagesAreReusedThenTheDeletedKeysStayGone) {
    constexpr int kCount = 200;

    AutoCommitKV db(path_);
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
        AutoCommitKV db(path_);
        db.open();
        for (int i = 0; i < kCount; ++i) {
            db.set(key(i), value(i));
        }
        for (int i = 0; i < kCount; ++i) {
            db.del(key(i));
        }
        db.close();
    }

    AutoCommitKV db(path_);
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

    AutoCommitKV db(path_);
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

    AutoCommitKV reopened(path_);
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
        AutoCommitKV db(path_);
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

    AutoCommitKV db(path_);
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
        AutoCommitKV db(path_);
        db.open();
        db.set(bytes("a"), bytes("1"));

        {
            ScopedFileSizeLimit limit(std::filesystem::file_size(path_));
            EXPECT_THROW(db.set(bytes("b"), bytes("2")), std::runtime_error);
        }

        db.set(bytes("b"), bytes("2"));
        db.close();
    }

    AutoCommitKV db(path_);
    db.open();
    EXPECT_EQ(db.get(bytes("a")), bytes("1"));
    EXPECT_EQ(db.get(bytes("b")), bytes("2"));
}

// ============================================================================
// Transactions
// ============================================================================
TEST_F(KVTest, WhenATransactionCommitsThenAReopenedKvSeesAllOfItsWrites) {
    constexpr int kCount = 200;
    {
        KV db(path_);
        db.open();
        KVTX tx;
        db.begin(&tx);
        for (int i = 0; i < kCount; ++i) {
            tx.set(key(i), value(i));
        }
        for (int i = 0; i < kCount; i += 2) {
            ASSERT_TRUE(tx.del(key(i)));
        }
        InsertReq req{key(1), bytes("updated"), UpdateMode::UPDATE_ONLY};
        ASSERT_TRUE(tx.update(&req));
        db.commit(&tx);
        db.close();
    }

    KV db(path_);
    db.open();
    KVTX tx;
    db.begin(&tx);
    EXPECT_EQ(tx.get(key(1)), bytes("updated"));
    for (int i = 2; i < kCount; ++i) {
        EXPECT_EQ(tx.get(key(i)), i % 2 == 0 ? std::nullopt : std::optional(value(i))) << "key " << i;
    }
    db.abort(&tx);
}

TEST_F(KVTest, WhenATransactionAbortsAfterManyWritesThenTheFileAndTheTreeAreUnchanged) {
    constexpr int kCount = 100;
    KV db(path_);
    db.open();
    KVTX setup;
    db.begin(&setup);
    for (int i = 0; i < kCount; ++i) {
        setup.set(key(i), value(i));
    }
    db.commit(&setup);
    const std::string before = file_contents(path_);

    KVTX tx;
    db.begin(&tx);
    const uint64_t root = live_root(tx);
    for (int i = 0; i < kCount; i += 2) {
        ASSERT_TRUE(tx.del(key(i)));
    }
    for (int i = kCount; i < 3 * kCount; ++i) {
        tx.set(key(i), value(i));
    }
    InsertReq update{key(1), bytes("updated"), UpdateMode::UPDATE_ONLY};
    ASSERT_TRUE(tx.update(&update));
    DeleteReq removal{key(3)};
    ASSERT_TRUE(tx.del(&removal));
    EXPECT_EQ(tx.get(key(0)), std::nullopt); // the tx reads its own writes
    EXPECT_EQ(tx.get(key(1)), bytes("updated"));
    db.abort(&tx);

    EXPECT_EQ(file_contents(path_), before);
    auto expect_original = [&](KV& kv) {
        KVTX after;
        kv.begin(&after);
        EXPECT_EQ(live_root(after), root);
        for (int i = 0; i < 3 * kCount; ++i) {
            EXPECT_EQ(after.get(key(i)), i < kCount ? std::optional(value(i)) : std::nullopt) << "key " << i;
        }
        kv.abort(&after);
    };
    expect_original(db);
    db.close();

    KV reopened(path_);
    reopened.open();
    expect_original(reopened);
}

TEST_F(KVTest, WhenATransactionMakesNoWritesThenCommitLeavesTheFileUntouched) {
    KV db(path_);
    db.open();
    KVTX setup;
    db.begin(&setup);
    setup.set(bytes("a"), bytes("1"));
    db.commit(&setup);

    // Backdate the file, so any write during the commit would move its mtime.
    std::filesystem::last_write_time(path_, std::filesystem::file_time_type::clock::now() - std::chrono::hours(24));
    const std::filesystem::file_time_type mtime = std::filesystem::last_write_time(path_);
    const std::string before = file_contents(path_);

    KVTX tx;
    db.begin(&tx);
    EXPECT_EQ(tx.get(bytes("a")), bytes("1"));
    EXPECT_TRUE(tx.seek(bytes("a"), CMP_GE).valid());
    InsertReq insert{bytes("a"), bytes("2"), UpdateMode::INSERT_ONLY};
    EXPECT_FALSE(tx.update(&insert)); // a rejected write changes nothing
    EXPECT_FALSE(tx.del(bytes("missing")));
    db.commit(&tx);

    EXPECT_EQ(std::filesystem::last_write_time(path_), mtime);
    EXPECT_EQ(file_contents(path_), before);

    // The mtime check does catch a commit that writes.
    KVTX write;
    db.begin(&write);
    write.set(bytes("b"), bytes("2"));
    db.commit(&write);
    EXPECT_NE(std::filesystem::last_write_time(path_), mtime);
}

TEST_F(KVTest, WhenWritingPagesFailsThenCommitRollsBackToTheRootAtBegin) {
    constexpr int kCount = 50;
    KV db(path_);
    db.open();
    KVTX setup;
    db.begin(&setup);
    for (int i = 0; i < kCount; ++i) {
        setup.set(key(i), value(i));
    }
    db.commit(&setup);
    const std::string meta_page = file_contents(path_).substr(0, BTREE_PAGE_SIZE);

    KVTX tx;
    db.begin(&tx);
    const uint64_t root = live_root(tx);
    for (int i = 0; i < kCount; i += 2) {
        ASSERT_TRUE(tx.del(key(i)));
    }
    for (int i = kCount; i < 2 * kCount; ++i) {
        tx.set(key(i), value(i));
    }
    ASSERT_NE(live_root(tx), root);
    {
        // Capped below the tree's pages, so writing any page of the tx fails.
        ScopedFileSizeLimit limit(2 * BTREE_PAGE_SIZE);
        EXPECT_THROW(db.commit(&tx), std::runtime_error);
    }

    EXPECT_EQ(file_contents(path_).substr(0, BTREE_PAGE_SIZE), meta_page);
    KVTX after;
    db.begin(&after);
    EXPECT_EQ(live_root(after), root);
    for (int i = 0; i < 2 * kCount; ++i) {
        EXPECT_EQ(after.get(key(i)), i < kCount ? std::optional(value(i)) : std::nullopt) << "key " << i;
    }

    // With the error gone, a new transaction commits on top of the restored version.
    for (int i = 0; i < kCount; i += 2) {
        ASSERT_TRUE(after.del(key(i)));
    }
    db.commit(&after);
    db.close();

    KV reopened(path_);
    reopened.open();
    KVTX check;
    reopened.begin(&check);
    for (int i = 0; i < 2 * kCount; ++i) {
        EXPECT_EQ(check.get(key(i)), i < kCount && i % 2 == 1 ? std::optional(value(i)) : std::nullopt) << "key " << i;
    }
    reopened.abort(&check);
}

TEST_F(KVTest, WhenWritesToANewFileAreAbortedThenALaterTransactionCanStillCommit) {
    KV db(path_);
    db.open();
    KVTX aborted;
    db.begin(&aborted);
    aborted.set(bytes("a"), bytes("1"));
    aborted.set(bytes("b"), bytes("2"));
    db.abort(&aborted);
    EXPECT_EQ(std::filesystem::file_size(path_), 0u);

    // The 2nd set frees the 1st one's root, updating the free list node that a new file only has in memory.
    KVTX tx;
    db.begin(&tx);
    tx.set(bytes("c"), bytes("3"));
    tx.set(bytes("d"), bytes("4"));
    db.commit(&tx);
    db.close();

    KV reopened(path_);
    reopened.open();
    KVTX check;
    reopened.begin(&check);
    EXPECT_EQ(check.get(bytes("a")), std::nullopt);
    EXPECT_EQ(check.get(bytes("b")), std::nullopt);
    EXPECT_EQ(check.get(bytes("c")), bytes("3"));
    EXPECT_EQ(check.get(bytes("d")), bytes("4"));
    reopened.abort(&check);
}

// Random transactions, each committed or aborted, checked against a std::map across reopens: an abort that missed any
// page or free list change would let a later commit hand out a page that is still in use.
TEST_F(KVTest, WhenRandomTransactionsCommitOrAbortThenOnlyTheCommittedWritesAreKept) {
    std::map<std::vector<uint8_t>, std::vector<uint8_t>> model;
    std::mt19937 rng(20260915);

    auto expect_model = [&](KV& kv) {
        KVTX tx;
        kv.begin(&tx);
        for (int k = 0; k < 150; ++k) {
            auto it = model.find(key(k));
            EXPECT_EQ(tx.get(key(k)), it == model.end() ? std::nullopt : std::optional(it->second)) << "key " << k;
        }
        kv.abort(&tx);
    };

    for (int session = 0; session < 3; ++session) {
        KV db(path_);
        db.open();
        for (int t = 0; t < 100; ++t) {
            std::map<std::vector<uint8_t>, std::vector<uint8_t>> pending = model;
            KVTX tx;
            db.begin(&tx);
            for (int ops = 1 + static_cast<int>(rng() % 20); ops > 0; --ops) {
                int k = static_cast<int>(rng() % 150);
                if (rng() % 3 == 0) {
                    ASSERT_EQ(tx.del(key(k)), pending.erase(key(k)) == 1) << "session " << session << ", key " << k;
                } else {
                    int v = static_cast<int>(rng() % 1000);
                    tx.set(key(k), value(v));
                    pending[key(k)] = value(v);
                }
            }
            if (rng() % 2 == 0) {
                db.commit(&tx);
                model = std::move(pending);
            } else {
                db.abort(&tx);
            }
        }
        expect_model(db);
        db.close();
    }

    KV db(path_);
    db.open();
    expect_model(db);
}
