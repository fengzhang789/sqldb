#include "catalog/catalog.h"
#include "catalog/tabledef.h"

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

TEST(TableDefBuilderTest, WhenAddIndexIsCalledThenIndexesKeepTheDeclaredColumnsWithoutPrefixes) {
    TableDef def = TableDefBuilder("t").add_col("id", INT_64).add_col("a", BYTES).add_col("b", INT_64)
        .set_pkeys(1).add_index({"b", "a"}).add_index({"a"}).build();

    EXPECT_EQ(def.indexes, (std::vector<std::vector<std::string>>{{"b", "a"}, {"a"}}));
    EXPECT_TRUE(def.index_prefixes.empty());
}

// ============================================================================
// col_index / check_index_keys / table_def_check
// ============================================================================
TEST(ColIndexTest, WhenAColumnExistsThenColIndexReturnsItsPositionElseMinusOne) {
    TableDef def = TableDefBuilder("t").add_col("id", INT_64).add_col("name", BYTES).build();
    EXPECT_EQ(col_index(def, "id"), 0);
    EXPECT_EQ(col_index(def, "name"), 1);
    EXPECT_EQ(col_index(def, "nope"), -1);
}

TEST(CheckIndexKeysTest, WhenAnIndexLacksPrimaryKeyColumnsThenCheckIndexKeysAppendsThemInPkOrder) {
    TableDef def = TableDefBuilder("t").add_col("a", BYTES).add_col("b", INT_64).add_col("c", BYTES)
        .add_col("d", INT_64).set_pkeys(2).build();
    std::vector<std::string> out;
    std::string err;

    ASSERT_TRUE(check_index_keys(def, {"c"}, &out, &err)) << err;
    EXPECT_EQ(out, (std::vector<std::string>{"c", "a", "b"}));

    ASSERT_TRUE(check_index_keys(def, {"d", "b"}, &out, &err)) << err;
    EXPECT_EQ(out, (std::vector<std::string>{"d", "b", "a"})); // b is already there
}

TEST(CheckIndexKeysTest, WhenAnIndexIsInvalidThenCheckIndexKeysFails) {
    TableDef def = TableDefBuilder("t").add_col("a", BYTES).add_col("b", INT_64).add_col("c", BYTES)
        .add_col("d", INT_64).set_pkeys(2).build();
    const std::vector<std::string> bad[] = {
        {},
        {"nope"},
        {"c", "c"},
        {"c", "d"}, // plus the pk, covers every column
    };
    for (const auto& index : bad) {
        std::vector<std::string> out;
        std::string err;
        EXPECT_FALSE(check_index_keys(def, index, &out, &err)) << index.size();
        EXPECT_FALSE(err.empty());
    }
}

TEST(TableDefCheckTest, WhenIndexesAreValidThenTableDefCheckNormalizesEachInPlace) {
    TableDef def = TableDefBuilder("t").add_col("id", INT_64).add_col("a", BYTES).add_col("b", INT_64)
        .set_pkeys(1).add_index({"a"}).add_index({"b"}).build();
    std::string err;
    ASSERT_TRUE(table_def_check(&def, &err)) << err;
    EXPECT_EQ(def.indexes, (std::vector<std::vector<std::string>>{{"a", "id"}, {"b", "id"}}));
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

// ============================================================================
// Catalog::table_new with secondary indexes
// ============================================================================
TEST_F(CatalogTest, WhenATableHasIndexesThenEachIndexGetsTheNextPrefixAfterTheTables) {
    Catalog catalog(kv_.get());
    std::string err;
    TableDef users = TableDefBuilder("users").add_col("id", INT_64).add_col("name", BYTES).add_col("age", INT_64)
        .set_pkeys(1).add_index({"name"}).add_index({"age"}).build();
    TableDef next = TableDefBuilder("next").add_col("id", INT_64).set_pkeys(1).build();
    ASSERT_TRUE(catalog.table_new(users, &err)) << err;
    ASSERT_TRUE(catalog.table_new(next, &err)) << err;

    const TableDef* u = catalog.get_table_def("users");
    const TableDef* n = catalog.get_table_def("next");
    ASSERT_NE(u, nullptr);
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(u->prefix, TABLE_PREFIX_MIN);
    EXPECT_EQ(u->index_prefixes, (std::vector<uint32_t>{TABLE_PREFIX_MIN + 1, TABLE_PREFIX_MIN + 2}));
    EXPECT_EQ(n->prefix, TABLE_PREFIX_MIN + 3);
    EXPECT_TRUE(n->index_prefixes.empty());
}

TEST_F(CatalogTest, WhenATableWithIndexesIsReopenedThenItsNormalizedIndexesAndPrefixesSurvive) {
    std::string err;
    {
        Catalog catalog(kv_.get());
        TableDef t = TableDefBuilder("t").add_col("id", INT_64).add_col("name", BYTES).add_col("age", INT_64)
            .add_col("bio", BYTES).set_pkeys(1).add_index({"name"}).add_index({"age", "name"}).build();
        ASSERT_TRUE(catalog.table_new(t, &err)) << err;
    }

    Catalog reopened(kv_.get());
    const TableDef* d = reopened.get_table_def("t");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->cols, (std::vector<std::string>{"id", "name", "age", "bio"}));
    EXPECT_EQ(d->indexes, (std::vector<std::vector<std::string>>{{"name", "id"}, {"age", "name", "id"}}));
    EXPECT_EQ(d->index_prefixes, (std::vector<uint32_t>{TABLE_PREFIX_MIN + 1, TABLE_PREFIX_MIN + 2}));
}

TEST_F(CatalogTest, WhenAnIndexIsInvalidThenTableNewFailsWithoutAllocatingAPrefix) {
    Catalog catalog(kv_.get());
    std::string err;
    TableDef bad = TableDefBuilder("bad").add_col("id", INT_64).add_col("name", BYTES).add_col("age", INT_64)
        .set_pkeys(1).add_index({"missing"}).build();
    EXPECT_FALSE(catalog.table_new(bad, &err));
    EXPECT_FALSE(err.empty());
    EXPECT_EQ(catalog.get_table_def("bad"), nullptr);

    err.clear();
    TableDef good = TableDefBuilder("good").add_col("id", INT_64).set_pkeys(1).build();
    ASSERT_TRUE(catalog.table_new(good, &err)) << err;
    EXPECT_EQ(catalog.get_table_def("good")->prefix, TABLE_PREFIX_MIN);
}

TEST_F(CatalogTest, WhenTheCallerSetsIndexPrefixesThenTableNewFails) {
    Catalog catalog(kv_.get());
    std::string err;
    TableDef t = TableDefBuilder("t").add_col("id", INT_64).add_col("name", BYTES).add_col("age", INT_64)
        .set_pkeys(1).add_index({"name"}).build();
    t.index_prefixes = {42};
    EXPECT_FALSE(catalog.table_new(t, &err));
    EXPECT_FALSE(err.empty());
}
