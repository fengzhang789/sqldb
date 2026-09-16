#include "access/scanner.h"

#include "access/table_access.h"
#include "catalog/tabledef.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {
    constexpr int64_t I64_MIN = std::numeric_limits<int64_t>::min();
    constexpr int64_t I64_MAX = std::numeric_limits<int64_t>::max();

    using Ids = std::vector<int64_t>;

    bool satisfies(int64_t id, CMP cmp, int64_t ref) {
        switch (cmp) {
            case CMP_GE: return id >= ref;
            case CMP_GT: return id > ref;
            case CMP_LT: return id < ref;
            case CMP_LE: return id <= ref;
        }
        return false;
    }

    TableDef id_name_table(const std::string& name, uint32_t prefix) {
        return TableDefBuilder(name).add_col("id", INT_64).add_col("name", BYTES).set_pkeys(1).set_prefix(prefix).build();
    }

    std::string name_for(int64_t id) {
        return "user" + std::to_string(id) + std::string(100, 'x'); // long enough that rows span several leaves
    }

    Record pk(int64_t id) {
        Record rec;
        rec.add_int64("id", id);
        return rec;
    }

    class ScannerTest : public ::testing::Test {
    protected:
        void SetUp() override {
            const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
            path_ = (std::filesystem::temp_directory_path() /
                     (std::string("scanner_test_") + info->test_suite_name() + "_" + info->name() + ".db"))
                        .string();
            std::filesystem::remove(path_);
            kv_ = std::make_unique<KV>(path_);
            kv_->open();
        }

        void TearDown() override {
            kv_.reset();
            std::filesystem::remove(path_);
        }

        // Runs one access call in its own committed transaction, as each call was durable on its own before ch. 11.
        template <typename Fn, typename... Args>
        bool in_tx(Fn fn, Args&&... args) {
            KVTX tx;
            kv_->begin(&tx);
            bool ok = fn(&tx, std::forward<Args>(args)...);
            kv_->commit(&tx);
            return ok;
        }

        // Runs one read-only access call in its own read transaction.
        template <typename Fn, typename... Args>
        bool in_reader(Fn fn, Args&&... args) {
            KVReader tx;
            kv_->begin_read(&tx);
            bool ok = fn(&tx, std::forward<Args>(args)...);
            kv_->end_read(&tx);
            return ok;
        }

        void upsert(const TableDef& tdef, const Record& rec) {
            std::string err;
            ASSERT_TRUE(in_tx(db_update, tdef, rec, UpdateMode::UPSERT, &err)) << err;
        }

        void upsert_id(const TableDef& tdef, int64_t id, const std::string& name) {
            Record rec;
            rec.add_int64("id", id).add_str("name", name);
            upsert(tdef, rec);
        }

        // users_ gets ids -100, -98, ..., 100 (not in key order); the neighbouring prefixes get rows a scan must skip.
        void insert_rows() {
            for (int64_t id = 0; id <= 100; id += 2) {
                upsert_id(users_, id, name_for(id));
                if (id > 0) {
                    upsert_id(users_, -id, name_for(-id));
                }
            }
            for (int64_t id : {I64_MIN, int64_t{0}, I64_MAX}) {
                upsert_id(before_, id, "before");
                upsert_id(after_, id, "after");
            }
        }

        static Ids all_ids() {
            Ids ids;
            for (int64_t id = -100; id <= 100; id += 2) {
                ids.push_back(id);
            }
            return ids;
        }

        // Scans users_, checking every row is intact, and returns the ids in scan order.
        Ids scan_ids(CMP cmp1, Record key1, CMP cmp2, Record key2) {
            KVReader tx;
            kv_->begin_read(&tx);
            Scanner sc(cmp1, cmp2, std::move(key1), std::move(key2));
            std::string err;
            EXPECT_TRUE(db_scan(&tx, users_, &sc, &err)) << err;

            Ids ids;
            for (; sc.valid(); sc.next()) {
                Record rec;
                sc.deref(&rec);
                ids.push_back(rec.get("id")->int64);
                EXPECT_EQ(rec.get("name")->str, name_for(ids.back()));
            }
            kv_->end_read(&tx);
            return ids;
        }

        Ids scan_ids(CMP cmp1, int64_t key1, CMP cmp2, int64_t key2) {
            return scan_ids(cmp1, pk(key1), cmp2, pk(key2));
        }

        std::string path_;
        std::unique_ptr<KV> kv_;
        TableDef before_ = id_name_table("before", 9);
        TableDef users_ = id_name_table("users", 10);
        TableDef after_ = id_name_table("after", 11);
    };
}

// ============================================================================
// Range shapes
// ============================================================================
TEST_F(ScannerTest, WhenForwardRangeIsClosedThenBothBoundsAreIncluded) {
    insert_rows();
    EXPECT_EQ(scan_ids(CMP_GE, -4, CMP_LE, 4), (Ids{-4, -2, 0, 2, 4}));
}

TEST_F(ScannerTest, WhenForwardRangeIsOpenThenBothBoundsAreExcluded) {
    insert_rows();
    EXPECT_EQ(scan_ids(CMP_GT, -4, CMP_LT, 4), (Ids{-2, 0, 2}));
}

TEST_F(ScannerTest, WhenForwardRangeIsHalfOpenThenOnlyTheInclusiveBoundIsIncluded) {
    insert_rows();
    EXPECT_EQ(scan_ids(CMP_GE, -4, CMP_LT, 4), (Ids{-4, -2, 0, 2}));
    EXPECT_EQ(scan_ids(CMP_GT, -4, CMP_LE, 4), (Ids{-2, 0, 2, 4}));
}

TEST_F(ScannerTest, WhenBackwardRangeIsClosedThenBothBoundsAreIncludedInDescendingOrder) {
    insert_rows();
    EXPECT_EQ(scan_ids(CMP_LE, 4, CMP_GE, -4), (Ids{4, 2, 0, -2, -4}));
}

TEST_F(ScannerTest, WhenBackwardRangeIsOpenThenBothBoundsAreExcludedInDescendingOrder) {
    insert_rows();
    EXPECT_EQ(scan_ids(CMP_LT, 4, CMP_GT, -4), (Ids{2, 0, -2}));
}

TEST_F(ScannerTest, WhenBackwardRangeIsHalfOpenThenOnlyTheInclusiveBoundIsIncludedInDescendingOrder) {
    insert_rows();
    EXPECT_EQ(scan_ids(CMP_LE, 4, CMP_GT, -4), (Ids{4, 2, 0, -2}));
    EXPECT_EQ(scan_ids(CMP_LT, 4, CMP_GE, -4), (Ids{2, 0, -2, -4}));
}

TEST_F(ScannerTest, WhenBoundsFallBetweenRowsThenInclusiveAndExclusiveCmpsReturnTheSameRows) {
    insert_rows();
    for (auto [lower, upper] : {std::pair{CMP_GE, CMP_LE}, std::pair{CMP_GT, CMP_LT}}) {
        EXPECT_EQ(scan_ids(lower, -3, upper, 3), (Ids{-2, 0, 2}));
        EXPECT_EQ(scan_ids(upper, 3, lower, -3), (Ids{2, 0, -2}));
    }
}

TEST_F(ScannerTest, WhenRangeCoversEveryInt64ThenOnlyThisTablesRowsComeBackInNumericOrder) {
    insert_rows();
    Ids ascending = all_ids();
    EXPECT_EQ(scan_ids(CMP_GE, I64_MIN, CMP_LE, I64_MAX), ascending);
    EXPECT_EQ(scan_ids(CMP_LE, I64_MAX, CMP_GE, I64_MIN), Ids(ascending.rbegin(), ascending.rend()));
}

TEST_F(ScannerTest, WhenScanningEveryBoundPairInEitherDirectionThenRowsMatchTheReference) {
    insert_rows();
    const int64_t bounds[] = {I64_MIN, -101, -100, -99, -1, 0, 1, 99, 100, 101, I64_MAX};
    const std::pair<CMP, CMP> cmp_pairs[] = {
        {CMP_GE, CMP_LE}, {CMP_GE, CMP_LT}, {CMP_GT, CMP_LE}, {CMP_GT, CMP_LT},
        {CMP_LE, CMP_GE}, {CMP_LE, CMP_GT}, {CMP_LT, CMP_GE}, {CMP_LT, CMP_GT},
    };
    for (auto [cmp1, cmp2] : cmp_pairs) {
        for (int64_t key1 : bounds) {
            for (int64_t key2 : bounds) {
                Ids expected;
                for (int64_t id : all_ids()) {
                    if (satisfies(id, cmp1, key1) && satisfies(id, cmp2, key2)) {
                        expected.push_back(id);
                    }
                }
                if (cmp1 < 0) {
                    std::reverse(expected.begin(), expected.end());
                }
                ASSERT_EQ(scan_ids(cmp1, key1, cmp2, key2), expected)
                    << "cmp1=" << cmp1 << " key1=" << key1 << " cmp2=" << cmp2 << " key2=" << key2;
            }
        }
    }
}

// ============================================================================
// Empty bounds: a full table scan
// ============================================================================
TEST_F(ScannerTest, WhenABoundIsEmptyThenItStandsForTheEndOfTheTable) {
    insert_rows();
    Ids ascending = all_ids();
    EXPECT_EQ(scan_ids(CMP_GE, Record{}, CMP_LE, Record{}), ascending);
    EXPECT_EQ(scan_ids(CMP_LE, Record{}, CMP_GE, Record{}), Ids(ascending.rbegin(), ascending.rend()));
    EXPECT_EQ(scan_ids(CMP_GE, Record{}, CMP_LE, pk(-96)), (Ids{-100, -98, -96}));
    EXPECT_EQ(scan_ids(CMP_LE, Record{}, CMP_GT, pk(96)), (Ids{100, 98}));
}

TEST_F(ScannerTest, WhenAnExclusiveStartBoundIsEmptyThenScanIsEmpty) {
    insert_rows();
    EXPECT_TRUE(scan_ids(CMP_GT, Record{}, CMP_LE, Record{}).empty());
    EXPECT_TRUE(scan_ids(CMP_LT, Record{}, CMP_GE, Record{}).empty());
}

// ============================================================================
// Empty results
// ============================================================================
TEST_F(ScannerTest, WhenRangeLiesStrictlyBetweenTwoAdjacentRowsThenScanIsEmpty) {
    insert_rows();
    EXPECT_TRUE(scan_ids(CMP_GT, 0, CMP_LT, 2).empty());
    EXPECT_TRUE(scan_ids(CMP_GE, 1, CMP_LE, 1).empty());
    EXPECT_TRUE(scan_ids(CMP_LT, 2, CMP_GT, 0).empty());
}

TEST_F(ScannerTest, WhenStartIsPastTheEndThenScanIsEmpty) {
    insert_rows();
    EXPECT_TRUE(scan_ids(CMP_GE, 10, CMP_LE, 4).empty());
    EXPECT_TRUE(scan_ids(CMP_GT, 4, CMP_LT, 4).empty());
    EXPECT_TRUE(scan_ids(CMP_LE, -10, CMP_GE, -4).empty());
}

TEST_F(ScannerTest, WhenRangeIsBeyondEitherEndOfTheTableThenScanIsEmpty) {
    insert_rows();
    EXPECT_TRUE(scan_ids(CMP_GT, 100, CMP_LE, I64_MAX).empty());
    EXPECT_TRUE(scan_ids(CMP_LT, -100, CMP_GE, I64_MIN).empty());
}

TEST_F(ScannerTest, WhenTableHasNoRowsThenScanIsEmpty) {
    EXPECT_TRUE(scan_ids(CMP_GE, I64_MIN, CMP_LE, I64_MAX).empty()); // the KV itself is empty

    upsert_id(before_, 0, "before");
    upsert_id(after_, 0, "after");
    EXPECT_TRUE(scan_ids(CMP_GE, I64_MIN, CMP_LE, I64_MAX).empty());
    EXPECT_TRUE(scan_ids(CMP_LE, I64_MAX, CMP_GE, I64_MIN).empty());
}

// ============================================================================
// Invalid requests
// ============================================================================
TEST_F(ScannerTest, WhenCmp1AndCmp2PointTheSameWayThenDbScanFailsAndLeavesTheScannerInvalid) {
    insert_rows();
    KVReader tx;
    kv_->begin_read(&tx);
    Scanner sc(CMP_GE, CMP_LE, pk(0), pk(10));
    std::string err;
    ASSERT_TRUE(db_scan(&tx, users_, &sc, &err)) << err;
    ASSERT_TRUE(sc.valid());

    for (auto [cmp1, cmp2] : {std::pair{CMP_GE, CMP_GT}, std::pair{CMP_LE, CMP_LT}}) {
        sc.cmp1 = cmp1;
        sc.cmp2 = cmp2;
        err.clear();
        EXPECT_FALSE(db_scan(&tx, users_, &sc, &err));
        EXPECT_FALSE(err.empty());
        EXPECT_FALSE(sc.valid());
    }
    kv_->end_read(&tx);
}

TEST_F(ScannerTest, WhenABoundIsNotAPrimaryKeyPrefixThenDbScanFails) {
    insert_rows();
    Record wrong_type;
    wrong_type.add_str("id", "0");
    Record extra_col = pk(0);
    extra_col.add_str("name", "x");
    Record non_key;
    non_key.add_str("name", "x");

    for (const Record& bad : {wrong_type, extra_col, non_key}) {
        for (bool bad_start : {true, false}) {
            Scanner sc(CMP_GE, CMP_LE, bad_start ? bad : pk(0), bad_start ? pk(10) : bad);
            std::string err;
            EXPECT_FALSE(in_reader(db_scan, users_, &sc, &err));
            EXPECT_FALSE(err.empty());
            EXPECT_FALSE(sc.valid());
        }
    }
}

// ============================================================================
// db_get: the degenerate scan [rec, rec]
// ============================================================================
TEST_F(ScannerTest, WhenDbGetHitsARowThenRecordHoldsTheFullRowInColumnOrder) {
    insert_rows();
    for (int64_t id : {-100, -2, 0, 100}) {
        Record rec = pk(id);
        std::string err;
        ASSERT_TRUE(in_reader(db_get,users_, &rec, &err)) << "id " << id << ": " << err;
        EXPECT_EQ(rec.cols, (std::vector<std::string>{"id", "name"}));
        EXPECT_EQ(rec.get("id")->int64, id);
        EXPECT_EQ(rec.get("name")->str, name_for(id));
    }
}

TEST_F(ScannerTest, WhenDbGetTargetsAnAbsentKeyThenItReturnsFalseWithoutAnError) {
    insert_rows();
    // Neighbours of existing rows, and keys that exist only in the adjacent tables.
    for (int64_t id : {int64_t{-101}, int64_t{-1}, int64_t{1}, int64_t{101}, I64_MIN, I64_MAX}) {
        Record rec = pk(id);
        std::string err;
        EXPECT_FALSE(in_reader(db_get,users_, &rec, &err)) << "id " << id;
        EXPECT_TRUE(err.empty());
    }
}

// ============================================================================
// Composite primary keys
// ============================================================================
TEST_F(ScannerTest, WhenPrimaryKeyIsCompositeThenRowsSortByEachColumnInTurn) {
    TableDef pairs = TableDefBuilder("pairs").add_col("a", BYTES).add_col("b", INT_64).add_col("c", INT_64)
        .set_pkeys(2).set_prefix(20).build();
    auto key = [](const std::string& a, int64_t b) {
        Record rec;
        rec.add_str("a", a).add_int64("b", b);
        return rec;
    };

    // Inserted out of order; c is each row's position in (a, b) order.
    const std::string x0{'x', '\x00'};
    const std::string x1{'x', '\x01'};
    const std::tuple<std::string, int64_t, int64_t> rows[] = {
        {"x", 5, 2}, {"y", I64_MIN, 5}, {x0, I64_MIN, 3}, {"", I64_MAX, 0}, {"x", -1, 1}, {x1, 0, 4},
    };
    for (const auto& [a, b, c] : rows) {
        Record rec = key(a, b);
        rec.add_int64("c", c);
        ASSERT_NO_FATAL_FAILURE(upsert(pairs, rec));
    }

    auto scan_c = [&](CMP cmp1, Record key1, CMP cmp2, Record key2) {
        KVReader tx;
        kv_->begin_read(&tx);
        Scanner sc(cmp1, cmp2, std::move(key1), std::move(key2));
        std::string err;
        EXPECT_TRUE(db_scan(&tx, pairs, &sc, &err)) << err;
        Ids cs;
        for (; sc.valid(); sc.next()) {
            Record rec;
            sc.deref(&rec);
            cs.push_back(rec.get("c")->int64);
        }
        kv_->end_read(&tx);
        return cs;
    };

    EXPECT_EQ(scan_c(CMP_GE, key("", I64_MIN), CMP_LT, key("z", I64_MIN)), (Ids{0, 1, 2, 3, 4, 5}));
    // "x\0" and "x\1" extend "x" but must not fall inside a range pinned to a == "x"
    EXPECT_EQ(scan_c(CMP_GE, key("x", I64_MIN), CMP_LE, key("x", I64_MAX)), (Ids{1, 2}));
    EXPECT_EQ(scan_c(CMP_LE, key("x", I64_MAX), CMP_GE, key("x", I64_MIN)), (Ids{2, 1}));

    // A bound naming only a is padded past every b for GT/LE, so it covers all of a's rows.
    auto a_only = [](const std::string& a) {
        Record rec;
        rec.add_str("a", a);
        return rec;
    };
    EXPECT_EQ(scan_c(CMP_GE, a_only("x"), CMP_LE, a_only("x")), (Ids{1, 2}));
    EXPECT_EQ(scan_c(CMP_GT, a_only("x"), CMP_LT, a_only("y")), (Ids{3, 4}));
    EXPECT_EQ(scan_c(CMP_LE, a_only("x"), CMP_GE, a_only("")), (Ids{2, 1, 0}));

    Record rec;
    rec.add_int64("b", 5).add_str("a", "x"); // pk columns out of tdef order
    std::string err;
    Scanner out_of_order(CMP_GE, CMP_LE, rec, rec);
    EXPECT_FALSE(in_reader(db_scan, pairs, &out_of_order, &err)); // scan bounds follow the index's column order...
    err.clear();
    ASSERT_TRUE(in_reader(db_get,pairs, &rec, &err)) << err; // ...but db_get takes pk columns in any order
    EXPECT_EQ(rec.cols, (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_EQ(rec.get("c")->int64, 2);
}
