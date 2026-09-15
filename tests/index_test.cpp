#include "access/table_access.h"

#include "access/row_codec.h"
#include "access/scanner.h"
#include "catalog/catalog.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <ostream>
#include <random>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {
    constexpr int64_t I64_MIN = std::numeric_limits<int64_t>::min();
    constexpr int64_t I64_MAX = std::numeric_limits<int64_t>::max();

    // Includes a string extending "nyc" and strings starting with 0xfe/0xff, which padded bounds must not leak into.
    const std::vector<std::string> CITIES = {
        "", "la", "nyc", std::string("nyc\0", 4), "sf", "\xfe", "\xff", std::string("\xff\0", 2), "\xff\xff",
    };

    const std::pair<CMP, CMP> CMP_PAIRS[] = {
        {CMP_GE, CMP_LE}, {CMP_GE, CMP_LT}, {CMP_GT, CMP_LE}, {CMP_GT, CMP_LT},
        {CMP_LE, CMP_GE}, {CMP_LE, CMP_GT}, {CMP_LT, CMP_GE}, {CMP_LT, CMP_GT},
    };

    struct Row {
        int64_t id;
        std::string name;
        int64_t age;
        std::string city;

        bool operator==(const Row&) const = default;
    };

    std::ostream& operator<<(std::ostream& os, const Row& row) {
        os << "{id=" << row.id << " name=" << row.name << " age=" << row.age << " city=";
        for (unsigned char c : row.city) {
            os << "\\x" << std::hex << static_cast<int>(c) << std::dec;
        }
        return os << "}";
    }

    std::string describe(const Record& rec) {
        std::ostringstream os;
        os << "{";
        for (size_t i = 0; i < rec.cols.size(); ++i) {
            os << rec.cols[i] << "=";
            if (rec.vals[i].type == INT_64) {
                os << rec.vals[i].int64 << " ";
                continue;
            }
            for (unsigned char c : rec.vals[i].str) {
                os << "\\x" << std::hex << static_cast<int>(c) << std::dec;
            }
            os << " ";
        }
        os << "}";
        return os.str();
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

    Record city_age_key(const std::string& city, int64_t age) {
        Record rec;
        rec.add_str("city", city).add_int64("age", age);
        return rec;
    }

    // Compares rec's columns named by bound to bound's values, column by column in bound's order.
    int compare_to_bound(const Record& rec, const Record& bound) {
        for (size_t i = 0; i < bound.cols.size(); ++i) {
            const Value& v = *rec.get(bound.cols[i]);
            const Value& b = bound.vals[i];
            int c = v.type == INT_64 ? (v.int64 > b.int64) - (v.int64 < b.int64) : v.str.compare(b.str);
            if (c != 0) {
                return c;
            }
        }
        return 0;
    }

    bool satisfies(int c, CMP cmp) {
        switch (cmp) {
            case CMP_GE: return c >= 0;
            case CMP_GT: return c > 0;
            case CMP_LT: return c < 0;
            case CMP_LE: return c <= 0;
        }
        return false;
    }

    // Key order of the pk (-1) and of the normalized indexes (age, id) and (city, age, id).
    bool index_less(int index_no, const Row& a, const Row& b) {
        switch (index_no) {
            case -1: return a.id < b.id;
            case 0: return std::tie(a.age, a.id) < std::tie(b.age, b.id);
            default: return std::tie(a.city, a.age, a.id) < std::tie(b.city, b.age, b.id);
        }
    }

    class IndexTest : public ::testing::Test {
    protected:
        void SetUp() override {
            const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
            path_ = (std::filesystem::temp_directory_path() /
                     (std::string("index_test_") + info->test_suite_name() + "_" + info->name() + ".db"))
                        .string();
            std::filesystem::remove(path_);
            kv_ = std::make_unique<KV>(path_);
            kv_->open();
            catalog_ = std::make_unique<Catalog>(kv_.get());

            TableDef def = TableDefBuilder("users")
                .add_col("id", INT_64)
                .add_col("name", BYTES)
                .add_col("age", INT_64)
                .add_col("city", BYTES)
                .set_pkeys(1)
                .add_index({"age"})
                .add_index({"city", "age"})
                .build();
            std::string err;
            ASSERT_TRUE(catalog_->table_new(def, &err)) << err;
            users_ = catalog_->get_table_def("users");
            ASSERT_NE(users_, nullptr);
        }

        void TearDown() override {
            catalog_.reset();
            kv_.reset();
            std::filesystem::remove(path_);
        }

        void upsert(const Row& row) {
            std::string err;
            ASSERT_TRUE(db_update(kv_.get(), *users_, to_record(row), UpdateMode::UPSERT, &err)) << err;
        }

        // 72 rows inserted out of id order, so every age in 20..29 and every city repeats.
        void insert_rows() {
            for (int64_t i = 0; i < 72; ++i) {
                int64_t id = (i * 37) % 72;
                Row row{id, "user" + std::to_string(id), 20 + (id * 7) % 10, CITIES[id % CITIES.size()]};
                ASSERT_NO_FATAL_FAILURE(upsert(row));
                rows_.push_back(row);
            }
        }

        // The raw keys stored under an index's prefix, checking each has an empty value.
        std::vector<std::string> index_keys(size_t index_no) {
            std::string prefix = encode_key(users_->index_prefixes[index_no], {});
            std::vector<std::string> keys;
            for (BIter it = kv_->seek(std::vector<uint8_t>(prefix.begin(), prefix.end()), CMP_GE); it.valid(); it.next()) {
                auto [key, val] = it.deref();
                std::string k(key.begin(), key.end());
                if (!k.starts_with(prefix)) {
                    break;
                }
                EXPECT_TRUE(val.empty());
                keys.push_back(std::move(k));
            }
            return keys;
        }

        // Each index holds exactly one key per row in `rows` and nothing else.
        void expect_indexes_match(const std::vector<Row>& rows) {
            for (size_t i = 0; i < users_->indexes.size(); ++i) {
                std::vector<std::string> expected;
                for (const Row& row : rows) {
                    Record rec = to_record(row);
                    std::vector<Value> vals;
                    for (const std::string& col : users_->indexes[i]) {
                        vals.push_back(*rec.get(col));
                    }
                    expected.push_back(encode_key(users_->index_prefixes[i], vals));
                }
                std::sort(expected.begin(), expected.end());
                EXPECT_EQ(index_keys(i), expected) << "index " << i;
            }
        }

        // Scans users_, checking the chosen index and that every row comes back whole, in tdef column order.
        std::vector<Row> scan(CMP cmp1, const Record& key1, CMP cmp2, const Record& key2, int index_no) {
            Scanner sc(cmp1, cmp2, key1, key2);
            std::string err;
            EXPECT_TRUE(db_scan(kv_.get(), *users_, &sc, &err)) << err;
            EXPECT_EQ(sc.index_no, index_no);

            std::vector<Row> rows;
            for (; sc.valid(); sc.next()) {
                Record rec;
                sc.deref(&rec);
                EXPECT_EQ(rec.cols, users_->cols);
                rows.push_back(from_record(rec));
            }
            return rows;
        }

        // rows_ within the range, in the order a scan over index_no should return them.
        std::vector<Row> reference(int index_no, CMP cmp1, const Record& key1, CMP cmp2, const Record& key2) {
            std::vector<Row> rows;
            for (const Row& row : rows_) {
                Record rec = to_record(row);
                if (satisfies(compare_to_bound(rec, key1), cmp1) && satisfies(compare_to_bound(rec, key2), cmp2)) {
                    rows.push_back(row);
                }
            }
            std::sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) { return index_less(index_no, a, b); });
            if (cmp1 < 0) {
                std::reverse(rows.begin(), rows.end());
            }
            return rows;
        }

        void expect_scan_matches(int index_no, CMP cmp1, const Record& key1, CMP cmp2, const Record& key2) {
            ASSERT_EQ(scan(cmp1, key1, cmp2, key2, index_no), reference(index_no, cmp1, key1, cmp2, key2))
                << "cmp1=" << cmp1 << " key1=" << describe(key1) << " cmp2=" << cmp2 << " key2=" << describe(key2);
        }

        std::string path_;
        std::unique_ptr<KV> kv_;
        std::unique_ptr<Catalog> catalog_;
        const TableDef* users_ = nullptr;
        std::vector<Row> rows_;
    };
}

// ============================================================================
// Index maintenance
// ============================================================================
TEST_F(IndexTest, WhenARowIsInsertedThenEachIndexGetsOneKeyWithAnEmptyValue) {
    Row alice{1, "alice", 30, "nyc"};
    upsert(alice);
    expect_indexes_match({alice});
}

TEST_F(IndexTest, WhenIndexedColumnsChangeThenTheOldIndexKeysAreReplaced) {
    upsert({1, "alice", 30, "nyc"});
    Row moved{1, "alice", 31, "sf"};
    upsert(moved);
    expect_indexes_match({moved});
}

TEST_F(IndexTest, WhenOnlyAnUnindexedColumnChangesThenIndexKeysStayTheSame) {
    upsert({1, "alice", 30, "nyc"});
    Row renamed{1, "alicia", 30, "nyc"};
    upsert(renamed);
    expect_indexes_match({renamed});
}

TEST_F(IndexTest, WhenARowIsDeletedThenItsIndexKeysAreRemoved) {
    Row alice{1, "alice", 30, "nyc"};
    Row bob{2, "bob", 30, "nyc"}; // shares every indexed value except the pk
    upsert(alice);
    upsert(bob);

    std::string err;
    ASSERT_TRUE(db_delete(kv_.get(), *users_, id_key(1), &err)) << err;
    expect_indexes_match({bob});
}

TEST_F(IndexTest, WhenAWriteIsRejectedThenIndexesAreUntouched) {
    Row alice{1, "alice", 30, "nyc"};
    upsert(alice);

    std::string err;
    EXPECT_FALSE(db_update(kv_.get(), *users_, to_record({1, "alice", 99, "la"}), UpdateMode::INSERT_ONLY, &err));
    EXPECT_FALSE(db_update(kv_.get(), *users_, to_record({2, "bob", 25, "sf"}), UpdateMode::UPDATE_ONLY, &err));
    EXPECT_FALSE(db_delete(kv_.get(), *users_, id_key(2), &err));
    expect_indexes_match({alice});
}

TEST_F(IndexTest, WhenARandomWorkloadRunsThenIndexesMatchTheLiveRows) {
    std::mt19937_64 rng(20260914);
    std::map<int64_t, Row> live;
    for (int i = 0; i < 300; ++i) {
        int64_t id = static_cast<int64_t>(rng() % 30);
        if (rng() % 4 == 0) {
            bool existed = live.erase(id) == 1;
            std::string err;
            ASSERT_EQ(db_delete(kv_.get(), *users_, id_key(id), &err), existed) << "delete id " << id;
        } else {
            Row row{id, "user" + std::to_string(rng() % 3), 20 + static_cast<int64_t>(rng() % 5),
                    CITIES[rng() % CITIES.size()]};
            ASSERT_NO_FATAL_FAILURE(upsert(row));
            live[id] = row;
        }
    }

    std::vector<Row> rows;
    for (const auto& [id, row] : live) {
        rows.push_back(row);
    }
    expect_indexes_match(rows);
}

// ============================================================================
// Scans through an index
// ============================================================================
TEST_F(IndexTest, WhenScanningAgeBoundPairsInEitherDirectionThenRowsMatchTheReference) {
    insert_rows();
    const int64_t bounds[] = {I64_MIN, 19, 20, 24, 29, 30, I64_MAX};
    for (auto [cmp1, cmp2] : CMP_PAIRS) {
        for (int64_t age1 : bounds) {
            for (int64_t age2 : bounds) {
                ASSERT_NO_FATAL_FAILURE(expect_scan_matches(0, cmp1, age_key(age1), cmp2, age_key(age2)));
            }
        }
    }
}

TEST_F(IndexTest, WhenBoundsNameOnlyTheCityThenEveryAgeAndIdForThatCityIsCovered) {
    insert_rows();
    std::vector<std::string> bounds = CITIES;
    for (const std::string& between : {std::string("a"), std::string("nyc\x01"), std::string("\xff\xff\xff")}) {
        bounds.push_back(between);
    }
    for (auto [cmp1, cmp2] : CMP_PAIRS) {
        for (const std::string& city1 : bounds) {
            for (const std::string& city2 : bounds) {
                ASSERT_NO_FATAL_FAILURE(expect_scan_matches(1, cmp1, city_key(city1), cmp2, city_key(city2)));
            }
        }
    }
}

TEST_F(IndexTest, WhenBoundsNameCityAndAgeThenOnlyTheIdIsPadded) {
    insert_rows();
    const std::tuple<CMP, Record, CMP, Record> cases[] = {
        {CMP_GE, city_age_key("nyc", 22), CMP_LE, city_age_key("nyc", 25)},
        {CMP_GT, city_age_key("la", 25), CMP_LT, city_age_key("sf", 21)},
        {CMP_LE, city_age_key("\xff", 29), CMP_GE, city_age_key("\xfe", 20)},
        {CMP_GE, city_age_key("nyc", 25), CMP_LE, city_key("nyc")}, // bounds of different lengths
        {CMP_LT, city_key("\xff"), CMP_GT, city_age_key("nyc", 27)},
    };
    for (const auto& [cmp1, key1, cmp2, key2] : cases) {
        ASSERT_NO_FATAL_FAILURE(expect_scan_matches(1, cmp1, key1, cmp2, key2));
    }
}

TEST_F(IndexTest, WhenKey1IsAPrimaryKeyPrefixThenDbScanUsesThePrimaryKey) {
    insert_rows();
    expect_scan_matches(-1, CMP_GE, Record{}, CMP_LE, Record{});
    expect_scan_matches(-1, CMP_GT, id_key(10), CMP_LE, id_key(20));
    expect_scan_matches(-1, CMP_LE, Record{}, CMP_GE, id_key(60));
}

// ============================================================================
// Invalid scans
// ============================================================================
TEST_F(IndexTest, WhenNoIndexStartsWithKey1ThenDbScanFails) {
    insert_rows();
    Record name;
    name.add_str("name", "user1");
    Record age_city;
    age_city.add_int64("age", 20).add_str("city", "nyc"); // (city, age) columns out of order
    Record city_id;
    city_id.add_str("city", "nyc").add_int64("id", 1); // skips age

    for (const Record& key : {name, age_city, city_id}) {
        Scanner sc(CMP_GE, CMP_LE, key, key);
        std::string err;
        EXPECT_FALSE(db_scan(kv_.get(), *users_, &sc, &err)) << describe(key);
        EXPECT_FALSE(err.empty());
        EXPECT_FALSE(sc.valid());
    }
}

TEST_F(IndexTest, WhenABoundDoesNotFitTheIndexKey1PicksThenDbScanFails) {
    insert_rows();
    Record wrong_type;
    wrong_type.add_str("age", "20");
    const std::pair<Record, Record> cases[] = {
        {age_key(20), city_key("nyc")},
        {city_key("nyc"), age_key(20)},
        {age_key(20), wrong_type},
        {wrong_type, age_key(20)},
    };
    for (const auto& [key1, key2] : cases) {
        Scanner sc(CMP_GE, CMP_LE, key1, key2);
        std::string err;
        EXPECT_FALSE(db_scan(kv_.get(), *users_, &sc, &err)) << describe(key1) << " " << describe(key2);
        EXPECT_FALSE(err.empty());
        EXPECT_FALSE(sc.valid());
    }
}
