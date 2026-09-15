#include "db.h"

#include <sys/resource.h>
#include <algorithm>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {
    constexpr int64_t I64_MIN = std::numeric_limits<int64_t>::min();
    constexpr int64_t I64_MAX = std::numeric_limits<int64_t>::max();

    struct Row {
        int64_t id;
        std::string name;
        int64_t age;
        std::string city;

        bool operator==(const Row&) const = default;
    };

    std::ostream& operator<<(std::ostream& os, const Row& row) {
        return os << "{id=" << row.id << " name=" << row.name << " age=" << row.age << " city=" << row.city << "}";
    }

    Record to_record(const Row& row) {
        Record rec;
        rec.add_int64("id", row.id).add_str("name", row.name).add_int64("age", row.age).add_str("city", row.city);
        return rec;
    }

    Row from_record(const Record& rec) {
        return {rec.get("id")->int64, rec.get("name")->str, rec.get("age")->int64, rec.get("city")->str};
    }

    Record id_key(int64_t id) {
        Record rec;
        rec.add_int64("id", id);
        return rec;
    }

    Record age_key(int64_t age) {
        Record rec;
        rec.add_int64("age", age);
        return rec;
    }

    Record city_key(const std::string& city) {
        Record rec;
        rec.add_str("city", city);
        return rec;
    }

    std::string file_contents(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
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

    class DBTest : public ::testing::Test {
    protected:
        void SetUp() override {
            const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
            path_ = (std::filesystem::temp_directory_path() /
                     (std::string("db_test_") + info->test_suite_name() + "_" + info->name() + ".db"))
                        .string();
            std::filesystem::remove(path_);
            reopen();

            // Indexes (age, id) and (city, age, id): every row write also writes 2 index keys.
            TableDef users = TableDefBuilder("users")
                .add_col("id", INT_64)
                .add_col("name", BYTES)
                .add_col("age", INT_64)
                .add_col("city", BYTES)
                .set_pkeys(1)
                .add_index({"age"})
                .add_index({"city", "age"})
                .build();
            DBTX tx;
            db_->begin(&tx);
            std::string err;
            ASSERT_TRUE(tx.table_new(users, &err)) << err;
            db_->commit(&tx);
        }

        void TearDown() override {
            db_.reset();
            kv_.reset();
            std::filesystem::remove(path_);
        }

        // Closes the file and opens it again, dropping all in-memory state like a restart.
        void reopen() {
            db_.reset();
            kv_.reset();
            kv_ = std::make_unique<KV>(path_);
            kv_->open();
            db_ = std::make_unique<DB>(kv_.get());
        }

        void insert(DBTX* tx, const Row& row) {
            std::string err;
            EXPECT_TRUE(tx->insert("users", to_record(row), &err)) << err;
        }

        void upsert(DBTX* tx, const Row& row) {
            std::string err;
            EXPECT_TRUE(tx->upsert("users", to_record(row), &err)) << err;
        }

        void del(DBTX* tx, int64_t id) {
            std::string err;
            EXPECT_TRUE(tx->del("users", id_key(id), &err)) << err;
        }

        void commit_rows(const std::vector<Row>& rows) {
            DBTX tx;
            db_->begin(&tx);
            for (const Row& row : rows) {
                insert(&tx, row);
            }
            db_->commit(&tx);
        }

        // The rows between key1 and key2 (inclusive), sorted by id; key1's columns pick the index.
        std::vector<Row> scan(DBTX* tx, const Record& key1, const Record& key2) {
            Scanner sc(CMP_GE, CMP_LE, key1, key2);
            std::string err;
            EXPECT_TRUE(tx->scan("users", &sc, &err)) << err;

            std::vector<Row> rows;
            for (; sc.valid(); sc.next()) {
                Record rec;
                sc.deref(&rec); // asserts on an index key without its row
                rows.push_back(from_record(rec));
            }
            std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.id < b.id; });
            return rows;
        }

        // tx sees exactly `rows`, through the primary key and through each index alike.
        void expect_rows(DBTX* tx, std::vector<Row> rows) {
            std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.id < b.id; });
            EXPECT_EQ(scan(tx, Record{}, Record{}), rows) << "primary key";
            EXPECT_EQ(scan(tx, age_key(I64_MIN), age_key(I64_MAX)), rows) << "(age, id) index";
            EXPECT_EQ(scan(tx, city_key(""), city_key("~")), rows) << "(city, age, id) index";
        }

        void expect_committed_rows(const std::vector<Row>& rows) {
            DBTX tx;
            db_->begin(&tx);
            expect_rows(&tx, rows);
            db_->abort(&tx);
        }

        std::string path_;
        std::unique_ptr<KV> kv_;
        std::unique_ptr<DB> db_;
    };
}

// ============================================================================
// Commit
// ============================================================================
TEST_F(DBTest, WhenATransactionCommitsThenAReopenedDbSeesEveryRowThroughEachIndex) {
    DBTX tx;
    db_->begin(&tx);
    insert(&tx, {1, "alice", 30, "nyc"});
    insert(&tx, {2, "bob", 25, "sf"});
    insert(&tx, {3, "carol", 30, "la"});
    upsert(&tx, {2, "bob", 26, "nyc"}); // replaces the index keys bob got earlier in this tx
    del(&tx, 3);
    db_->commit(&tx);

    const std::vector<Row> committed = {{1, "alice", 30, "nyc"}, {2, "bob", 26, "nyc"}};
    expect_committed_rows(committed);
    reopen();
    expect_committed_rows(committed);
}

// ============================================================================
// Abort
// ============================================================================
TEST_F(DBTest, WhenATransactionAbortsThenNoneOfItsRowOrIndexWritesSurvive) {
    const Row alice{1, "alice", 30, "nyc"};
    commit_rows({alice});
    const std::string before = file_contents(path_);

    DBTX tx;
    db_->begin(&tx);
    insert(&tx, {2, "bob", 25, "sf"});
    upsert(&tx, {1, "alice", 31, "la"}); // replaces alice's committed index keys
    insert(&tx, {3, "carol", 40, "nyc"});
    del(&tx, 3);
    expect_rows(&tx, {{1, "alice", 31, "la"}, {2, "bob", 25, "sf"}}); // the tx reads its own writes
    db_->abort(&tx);

    EXPECT_EQ(file_contents(path_), before);
    expect_committed_rows({alice});
    reopen();
    expect_committed_rows({alice});
}

TEST_F(DBTest, WhenAnIndexKeyIsTooLargeAfterItsRowIsWrittenThenAbortDropsTheRowToo) {
    const Row alice{1, "alice", 30, "nyc"};
    commit_rows({alice});

    // With a 985-byte city the row's 995-byte value fits, but its 1006-byte (city, age, id) key is over
    // BTREE_MAX_KEY_SIZE, so insert throws after writing the row and its (age, id) key.
    const Row big{2, "", 25, std::string(985, 'c')};
    DBTX tx;
    db_->begin(&tx);
    std::string err;
    EXPECT_THROW(tx.insert("users", to_record(big), &err), std::invalid_argument);
    db_->abort(&tx);

    expect_committed_rows({alice});
    reopen();
    expect_committed_rows({alice});
}

TEST_F(DBTest, WhenATableCreatedInATransactionIsAbortedThenLaterTransactionsCannotFindIt) {
    TableDef pets = TableDefBuilder("pets").add_col("id", INT_64).add_col("name", BYTES).set_pkeys(1).build();
    Record rex;
    rex.add_int64("id", 1).add_str("name", "rex");

    DBTX tx;
    db_->begin(&tx);
    std::string err;
    ASSERT_TRUE(tx.table_new(pets, &err)) << err;
    ASSERT_TRUE(tx.insert("pets", rex, &err)) << err; // caches the def this tx wrote
    db_->abort(&tx);

    DBTX next;
    db_->begin(&next);
    EXPECT_FALSE(next.insert("pets", rex, &err));
    EXPECT_EQ(err, "table not found: pets");
    db_->abort(&next);
    expect_committed_rows({}); // committed tables are still found
}

// ============================================================================
// Failed commit
// ============================================================================
TEST_F(DBTest, WhenCommitFailsWritingPagesThenNeitherTheRowsNorTheirIndexKeysAreKept) {
    const Row alice{1, "alice", 30, "nyc"};
    commit_rows({alice});

    DBTX tx;
    db_->begin(&tx);
    upsert(&tx, {1, "alice", 31, "la"});
    for (int64_t id = 2; id < 100; ++id) {
        insert(&tx, {id, "user" + std::to_string(id) + std::string(100, 'x'), 20 + id % 10, id % 2 == 0 ? "sf" : "la"});
    }
    {
        // Capped below the tree's pages, so writing any page of the tx fails.
        ScopedFileSizeLimit limit(2 * BTREE_PAGE_SIZE);
        EXPECT_THROW(db_->commit(&tx), std::runtime_error);
    }

    expect_committed_rows({alice});
    reopen();
    expect_committed_rows({alice});

    // A later transaction commits on top of the rolled back state.
    const Row bob{2, "bob", 25, "sf"};
    commit_rows({bob});
    reopen();
    expect_committed_rows({alice, bob});
}
