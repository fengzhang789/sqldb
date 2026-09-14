#include "catalog.h"
#include "tabledef.h"

#include <filesystem>
#include <memory>
#include <string>

#include <gtest/gtest.h>

namespace {
    class CatalogTest : public ::testing::Test {
    protected:
        void SetUp() override {
            const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
            path_ = (std::filesystem::temp_directory_path() /
                     (std::string("catalog_test_") + info->test_suite_name() + "_" + info->name() + ".db"))
                        .string();
            std::filesystem::remove(path_);
            kv_ = std::make_unique<KV>(path_);
            kv_->open();
        }

        void TearDown() override {
            kv_.reset();
            std::filesystem::remove(path_);
        }

        std::string path_;
        std::unique_ptr<KV> kv_;
    };
}

// ============================================================================
// TableDefBuilder
// ============================================================================
TEST(TableDefBuilderTest, WhenColumnsAreAddedThenTheyAppearInInsertionOrder) {
    TableDef def = TableDefBuilder("users")
        .add_col("id", INT_64)
        .add_col("name", BYTES)
        .add_col("age", INT_64)
        .build();

    ASSERT_EQ(def.cols.size(), 3u);
    EXPECT_EQ(def.cols[0], "id");
    EXPECT_EQ(def.types[0], INT_64);
    EXPECT_EQ(def.cols[1], "name");
    EXPECT_EQ(def.types[1], BYTES);
    EXPECT_EQ(def.cols[2], "age");
    EXPECT_EQ(def.types[2], INT_64);
}

TEST(TableDefBuilderTest, WhenSetPkeysIsCalledThenPkeysMatchesTheGivenCount) {
    TableDef def = TableDefBuilder("t")
        .add_col("a", BYTES)
        .add_col("b", INT_64)
        .add_col("c", BYTES)
        .set_pkeys(2)
        .build();

    EXPECT_EQ(def.pkeys, 2);
}

TEST(TableDefBuilderTest, WhenPkeysIsNotSetThenItDefaultsToZero) {
    TableDef def = TableDefBuilder("t").add_col("id", INT_64).build();
    EXPECT_EQ(def.pkeys, 0);
}

TEST(TableDefBuilderTest, WhenSetPrefixIsCalledThenPrefixMatchesTheGivenValue) {
    TableDef def = TableDefBuilder("t").add_col("id", INT_64).set_prefix(7).build();
    EXPECT_EQ(def.prefix, 7u);
}

TEST(TableDefBuilderTest, WhenPrefixIsNotSetThenItDefaultsToZero) {
    TableDef def = TableDefBuilder("t").add_col("id", INT_64).build();
    EXPECT_EQ(def.prefix, 0u);
}

// ============================================================================
// Catalog::table_new
// ============================================================================
TEST_F(CatalogTest, WhenMultipleTablesAreCreatedThenPrefixesIncrease) {
    Catalog catalog(kv_.get());
    std::string err;

    TableDef t1 = TableDefBuilder("t1").add_col("id", INT_64).set_pkeys(1).build();
    TableDef t2 = TableDefBuilder("t2").add_col("id", INT_64).set_pkeys(1).build();
    TableDef t3 = TableDefBuilder("t3").add_col("id", INT_64).set_pkeys(1).build();

    ASSERT_TRUE(catalog.table_new(t1, &err)) << err;
    ASSERT_TRUE(catalog.table_new(t2, &err)) << err;
    ASSERT_TRUE(catalog.table_new(t3, &err)) << err;

    const TableDef* d1 = catalog.get_table_def("t1");
    const TableDef* d2 = catalog.get_table_def("t2");
    const TableDef* d3 = catalog.get_table_def("t3");
    ASSERT_NE(d1, nullptr);
    ASSERT_NE(d2, nullptr);
    ASSERT_NE(d3, nullptr);

    EXPECT_EQ(d1->prefix, TABLE_PREFIX_MIN);
    EXPECT_EQ(d2->prefix, TABLE_PREFIX_MIN + 1);
    EXPECT_EQ(d3->prefix, TABLE_PREFIX_MIN + 2);
}

TEST_F(CatalogTest, WhenATableNameAlreadyExistsThenTableNewFails) {
    Catalog catalog(kv_.get());
    std::string err;

    TableDef t1 = TableDefBuilder("dup").add_col("id", INT_64).set_pkeys(1).build();
    ASSERT_TRUE(catalog.table_new(t1, &err)) << err;

    TableDef t2 = TableDefBuilder("dup").add_col("id", INT_64).add_col("other", BYTES).set_pkeys(1).build();
    EXPECT_FALSE(catalog.table_new(t2, &err));
    EXPECT_FALSE(err.empty());
}

TEST_F(CatalogTest, WhenTheSchemaIsInvalidThenTableNewFails) {
    Catalog catalog(kv_.get());
    std::string err;

    TableDef no_name = TableDefBuilder("").add_col("id", INT_64).set_pkeys(1).build();
    EXPECT_FALSE(catalog.table_new(no_name, &err));

    TableDef no_pkeys;
    no_pkeys.name = "no_pkeys";
    no_pkeys.cols = {"a"};
    no_pkeys.types = {INT_64};
    no_pkeys.pkeys = 0;
    EXPECT_FALSE(catalog.table_new(no_pkeys, &err));
}

TEST_F(CatalogTest, WhenTheTableIsUnknownThenGetTableDefReturnsNullptr) {
    Catalog catalog(kv_.get());
    EXPECT_EQ(catalog.get_table_def("nope"), nullptr);
}

TEST_F(CatalogTest, WhenGetTableDefIsCalledTwiceThenTheSecondCallHitsTheCache) {
    Catalog catalog(kv_.get());
    std::string err;
    TableDef t = TableDefBuilder("t").add_col("id", INT_64).add_col("name", BYTES).set_pkeys(1).build();
    ASSERT_TRUE(catalog.table_new(t, &err)) << err;

    const TableDef* first = catalog.get_table_def("t");  // populates the cache
    const TableDef* second = catalog.get_table_def("t");  // cache hit
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first, second);
    EXPECT_EQ(first->name, "t");
    EXPECT_EQ(first->cols, (std::vector<std::string>{"id", "name"}));
}

TEST_F(CatalogTest, WhenANewCatalogReadsTheSameKvThenTheTableDefSurvives) {
    std::string err;
    {
        Catalog catalog(kv_.get());
        TableDef t = TableDefBuilder("t").add_col("id", INT_64).add_col("name", BYTES).set_pkeys(1).build();
        ASSERT_TRUE(catalog.table_new(t, &err)) << err;
    }

    Catalog reopened(kv_.get());
    const TableDef* d = reopened.get_table_def("t");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->name, "t");
    EXPECT_EQ(d->pkeys, 1);
    EXPECT_EQ(d->prefix, TABLE_PREFIX_MIN);
}
