#include "ql/ql_range.h"

#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "catalog/catalog.h"
#include "catalog/tabledef.h"
#include "ql/ql_parse.h"

namespace {
    // Normalized the way Catalog::table_new would: check_index_keys appends the pk columns to every index.
    TableDef checked(TableDef def) {
        std::string err;
        EXPECT_TRUE(table_def_check(&def, &err)) << err;
        return def;
    }

    // Primary key (id), secondary indexes (age, id) and (city, age, id).
    TableDef users_def() {
        return checked(TableDefBuilder("users")
                           .add_col("id", INT_64)
                           .add_col("name", BYTES)
                           .add_col("age", INT_64)
                           .add_col("city", BYTES)
                           .set_pkeys(1)
                           .add_index({"age"})
                           .add_index({"city", "age"})
                           .build());
    }

    // Primary key (id) and nothing else.
    TableDef plain_def() {
        return checked(TableDefBuilder("plain")
                           .add_col("id", INT_64)
                           .add_col("name", BYTES)
                           .add_col("age", INT_64)
                           .set_pkeys(1)
                           .build());
    }

    QLNode filter_of(const std::string& where) {
        QLStatement stmt;
        std::string err;
        EXPECT_TRUE(parse_statement("SELECT id FROM users WHERE " + where, &stmt, &err)) << where << "\n" << err;
        return std::get<QLSelect>(stmt).scan.filter;
    }

    const char* cmp_name(CMP cmp) {
        switch (cmp) {
            case CMP_GE: return "GE";
            case CMP_GT: return "GT";
            case CMP_LT: return "LT";
            default: return "LE";
        }
    }

    std::string key_text(const Record& key) {
        std::string out;
        for (size_t i = 0; i < key.cols.size(); ++i) {
            if (i > 0) out += ",";
            out += key.cols[i] + "=";
            out += key.vals[i].type == INT_64 ? std::to_string(key.vals[i].int64) : key.vals[i].str;
        }
        return out;
    }

    // "GT(id=5) LE()" - the bounds a WHERE clause narrows the scan to. "GE() LE()" is the whole table.
    std::string plan(const std::string& where, const TableDef& tdef) {
        QLRange r = ql_range(filter_of(where), tdef);
        return std::string(cmp_name(r.cmp1)) + "(" + key_text(r.key1) + ") " + cmp_name(r.cmp2) + "(" +
               key_text(r.key2) + ")";
    }

    constexpr const char* FULL_SCAN = "GE() LE()";
}

// ============================================================================
// The primary key
// ============================================================================
TEST(QLRangeTest, WhenThereIsNoFilterThenTheWholeTableIsScanned) {
    QLRange r = ql_range(QLNode{}, users_def());

    EXPECT_TRUE(r.key1.cols.empty());
    EXPECT_TRUE(r.key2.cols.empty());
    EXPECT_EQ(r.cmp1, CMP_GE);
    EXPECT_EQ(r.cmp2, CMP_LE);
}

TEST(QLRangeTest, WhenThePrimaryKeyIsFixedThenTheRangeIsAPointQuery) {
    EXPECT_EQ(plan("id = 1", users_def()), "GE(id=1) LE(id=1)");
}

TEST(QLRangeTest, WhenThePrimaryKeyHasALowerBoundThenOnlyTheStartIsPinned) {
    EXPECT_EQ(plan("id > 5", users_def()), "GT(id=5) LE()");
    EXPECT_EQ(plan("id >= 5", users_def()), "GE(id=5) LE()");
}

TEST(QLRangeTest, WhenThePrimaryKeyHasAnUpperBoundThenOnlyTheEndIsPinned) {
    EXPECT_EQ(plan("id < 5", users_def()), "GE() LT(id=5)");
    EXPECT_EQ(plan("id <= 5", users_def()), "GE() LE(id=5)");
}

TEST(QLRangeTest, WhenBothEndsAreBoundedThenTheRangeIsClosed) {
    EXPECT_EQ(plan("id >= 2 AND id <= 8", users_def()), "GE(id=2) LE(id=8)");
    EXPECT_EQ(plan("id > 2 AND id < 8", users_def()), "GT(id=2) LT(id=8)");
}

TEST(QLRangeTest, WhenTheColumnIsOnTheRightThenTheComparisonIsFlipped) {
    EXPECT_EQ(plan("5 < id", users_def()), "GT(id=5) LE()");
    EXPECT_EQ(plan("5 >= id", users_def()), "GE() LE(id=5)");
}

TEST(QLRangeTest, WhenTheValueIsAnExpressionThenItIsFolded) {
    EXPECT_EQ(plan("id = 1 + 2", users_def()), "GE(id=3) LE(id=3)");
}

TEST(QLRangeTest, WhenAColumnIsBoundedTwiceThenTheTighterBoundWins) {
    EXPECT_EQ(plan("id > 2 AND id > 5", users_def()), "GT(id=5) LE()");
    EXPECT_EQ(plan("id < 9 AND id < 4", users_def()), "GE() LT(id=4)");
}

// ============================================================================
// Secondary indexes
// ============================================================================
TEST(QLRangeTest, WhenAnIndexedColumnIsBoundedThenThatIndexIsUsed) {
    EXPECT_EQ(plan("age >= 30", users_def()), "GE(age=30) LE()");
    EXPECT_EQ(plan("age = 30", users_def()), "GE(age=30) LE(age=30)");
}

// find_index reads the index off key1, so an upper bound alone would be checked against the primary key instead.
TEST(QLRangeTest, WhenAnIndexedColumnHasOnlyAnUpperBoundThenTheScanFallsBack) {
    EXPECT_EQ(plan("age < 30", users_def()), FULL_SCAN);
}

TEST(QLRangeTest, WhenAnIndexPrefixIsFixedThenTheNextColumnSuppliesTheRange) {
    EXPECT_EQ(plan("city = 'nyc' AND age > 20", users_def()), "GT(city=nyc,age=20) LE(city=nyc)");
    EXPECT_EQ(plan("city = 'nyc' AND age <= 40", users_def()), "GE(city=nyc) LE(city=nyc,age=40)");
}

TEST(QLRangeTest, WhenOnlyTheLeadingIndexColumnIsFixedThenTheWholePrefixIsScanned) {
    EXPECT_EQ(plan("city = 'nyc'", users_def()), "GE(city=nyc) LE(city=nyc)");
}

TEST(QLRangeTest, WhenTheFixedColumnIsNotAnIndexPrefixThenTheScanFallsBack) {
    // (city, age, id) cannot be entered by age alone, and this table has no index on name.
    EXPECT_EQ(plan("name = 'ann'", users_def()), FULL_SCAN);
    EXPECT_EQ(plan("age = 30", plain_def()), FULL_SCAN);
}

TEST(QLRangeTest, WhenTheKeyAndAnIndexBothFitThenThePrimaryKeyWins) {
    EXPECT_EQ(plan("id = 1 AND age > 5", users_def()), "GE(id=1) LE(id=1)");
}

// ============================================================================
// What must not narrow a scan
// ============================================================================
TEST(QLRangeTest, WhenConditionsAreOredThenNothingIsNarrowed) {
    EXPECT_EQ(plan("id = 1 OR id = 2", users_def()), FULL_SCAN);
    EXPECT_EQ(plan("id > 5 OR age < 3", users_def()), FULL_SCAN);
}

TEST(QLRangeTest, WhenAConditionIsNegatedThenNothingIsNarrowed) {
    EXPECT_EQ(plan("NOT id = 1", users_def()), FULL_SCAN);
}

TEST(QLRangeTest, WhenTheComparisonIsNotEqualThenNothingIsNarrowed) {
    EXPECT_EQ(plan("id != 1", users_def()), FULL_SCAN);
}

TEST(QLRangeTest, WhenBothSidesAreColumnsThenNothingIsNarrowed) {
    EXPECT_EQ(plan("id = age", users_def()), FULL_SCAN);
}

TEST(QLRangeTest, WhenTheLiteralTypeMismatchesTheColumnThenNothingIsNarrowed) {
    EXPECT_EQ(plan("id = 'x'", users_def()), FULL_SCAN);
}

TEST(QLRangeTest, WhenTheColumnIsUnknownThenNothingIsNarrowed) {
    EXPECT_EQ(plan("nope = 1", users_def()), FULL_SCAN);
}

TEST(QLRangeTest, WhenAnOredConditionSitsBesideAUsableOneThenOnlyTheUsableOneNarrows) {
    EXPECT_EQ(plan("id = 1 AND (age = 2 OR age = 3)", users_def()), "GE(id=1) LE(id=1)");
    EXPECT_EQ(plan("id > 5 AND name = 'ann'", users_def()), "GT(id=5) LE()");
}
