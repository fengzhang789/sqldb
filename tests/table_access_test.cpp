#include "access/table_access.h"

#include "catalog/tabledef.h"

#include <filesystem>
#include <memory>
#include <string>

#include <gtest/gtest.h>

namespace {
    class TableAccessTest : public ::testing::Test {
    protected:
        void SetUp() override {
            const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
            path_ = (std::filesystem::temp_directory_path() /
                     (std::string("table_access_test_") + info->test_suite_name() + "_" + info->name() + ".db"))
                        .string();
            std::filesystem::remove(path_);
            kv_ = std::make_unique<KV>(path_);
            kv_->open();

            tdef_ = TableDefBuilder("users")
                .add_col("id", INT_64)
                .add_col("name", BYTES)
                .add_col("age", INT_64)
                .set_pkeys(1)
                .set_prefix(10)
                .build();
        }

        void TearDown() override {
            kv_.reset();
            std::filesystem::remove(path_);
        }

        Record full_row(int64_t id, const std::string& name, int64_t age) {
            Record rec;
            rec.add_int64("id", id).add_str("name", name).add_int64("age", age);
            return rec;
        }

        Record pk_only(int64_t id) {
            Record rec;
            rec.add_int64("id", id);
            return rec;
        }

        std::string path_;
        std::unique_ptr<KV> kv_;
        TableDef tdef_;
    };
}

TEST_F(TableAccessTest, WhenARowIsUpsertedThenDbGetRoundTripsIt) {
    std::string err;
    ASSERT_TRUE(db_update(kv_.get(), tdef_, full_row(1, "alice", 30), UpdateMode::UPSERT, &err)) << err;

    Record rec = pk_only(1);
    ASSERT_TRUE(db_get(kv_.get(), tdef_, &rec, &err)) << err;

    const Value* name = rec.get("name");
    const Value* age = rec.get("age");
    ASSERT_NE(name, nullptr);
    ASSERT_NE(age, nullptr);
    EXPECT_EQ(name->str, "alice");
    EXPECT_EQ(age->int64, 30);
}

TEST_F(TableAccessTest, WhenTheRowIsMissingThenDbGetReturnsFalseWithNoError) {
    std::string err;
    Record rec = pk_only(999);
    EXPECT_FALSE(db_get(kv_.get(), tdef_, &rec, &err));
    EXPECT_TRUE(err.empty());
}

TEST_F(TableAccessTest, WhenTheRowAlreadyExistsThenUpsertOverwritesIt) {
    std::string err;
    ASSERT_TRUE(db_update(kv_.get(), tdef_, full_row(1, "alice", 30), UpdateMode::UPSERT, &err)) << err;
    ASSERT_TRUE(db_update(kv_.get(), tdef_, full_row(1, "alice", 31), UpdateMode::UPSERT, &err)) << err;

    Record rec = pk_only(1);
    ASSERT_TRUE(db_get(kv_.get(), tdef_, &rec, &err)) << err;
    EXPECT_EQ(rec.get("age")->int64, 31);
}

TEST_F(TableAccessTest, WhenTheRowAlreadyExistsThenInsertOnlyFails) {
    std::string err;
    ASSERT_TRUE(db_update(kv_.get(), tdef_, full_row(1, "alice", 30), UpdateMode::INSERT_ONLY, &err)) << err;
    EXPECT_FALSE(db_update(kv_.get(), tdef_, full_row(1, "alice", 31), UpdateMode::INSERT_ONLY, &err));
    EXPECT_FALSE(err.empty());

    // The failed insert must not have overwritten the row.
    Record rec = pk_only(1);
    ASSERT_TRUE(db_get(kv_.get(), tdef_, &rec, &err)) << err;
    EXPECT_EQ(rec.get("age")->int64, 30);
}

TEST_F(TableAccessTest, WhenTheRowDoesNotExistThenInsertOnlySucceeds) {
    std::string err;
    EXPECT_TRUE(db_update(kv_.get(), tdef_, full_row(1, "alice", 30), UpdateMode::INSERT_ONLY, &err)) << err;
}

TEST_F(TableAccessTest, WhenTheRowDoesNotExistThenUpdateOnlyFails) {
    std::string err;
    EXPECT_FALSE(db_update(kv_.get(), tdef_, full_row(1, "alice", 30), UpdateMode::UPDATE_ONLY, &err));
    EXPECT_FALSE(err.empty());

    Record rec = pk_only(1);
    EXPECT_FALSE(db_get(kv_.get(), tdef_, &rec, &err));
}

TEST_F(TableAccessTest, WhenTheRowAlreadyExistsThenUpdateOnlySucceeds) {
    std::string err;
    ASSERT_TRUE(db_update(kv_.get(), tdef_, full_row(1, "alice", 30), UpdateMode::UPSERT, &err)) << err;
    EXPECT_TRUE(db_update(kv_.get(), tdef_, full_row(1, "alice", 31), UpdateMode::UPDATE_ONLY, &err)) << err;

    Record rec = pk_only(1);
    ASSERT_TRUE(db_get(kv_.get(), tdef_, &rec, &err)) << err;
    EXPECT_EQ(rec.get("age")->int64, 31);
}

TEST_F(TableAccessTest, WhenDbDeleteIsCalledThenTheRowIsRemoved) {
    std::string err;
    ASSERT_TRUE(db_update(kv_.get(), tdef_, full_row(1, "alice", 30), UpdateMode::UPSERT, &err)) << err;

    EXPECT_TRUE(db_delete(kv_.get(), tdef_, pk_only(1), &err));

    Record rec = pk_only(1);
    EXPECT_FALSE(db_get(kv_.get(), tdef_, &rec, &err));
}

TEST_F(TableAccessTest, WhenTheRowIsMissingThenDbDeleteReturnsFalse) {
    std::string err;
    EXPECT_FALSE(db_delete(kv_.get(), tdef_, pk_only(404), &err));
}

TEST_F(TableAccessTest, WhenARequiredColumnIsMissingThenDbUpdateFails) {
    std::string err;
    Record incomplete;
    incomplete.add_int64("id", 1).add_str("name", "alice");  // missing "age"

    EXPECT_FALSE(db_update(kv_.get(), tdef_, incomplete, UpdateMode::UPSERT, &err));
    EXPECT_FALSE(err.empty());
}

TEST_F(TableAccessTest, WhenMultipleRowsExistThenEachIsIndependentlyAddressable) {
    std::string err;
    ASSERT_TRUE(db_update(kv_.get(), tdef_, full_row(1, "alice", 30), UpdateMode::UPSERT, &err)) << err;
    ASSERT_TRUE(db_update(kv_.get(), tdef_, full_row(2, "bob", 25), UpdateMode::UPSERT, &err)) << err;

    Record r1 = pk_only(1);
    Record r2 = pk_only(2);
    ASSERT_TRUE(db_get(kv_.get(), tdef_, &r1, &err)) << err;
    ASSERT_TRUE(db_get(kv_.get(), tdef_, &r2, &err)) << err;

    EXPECT_EQ(r1.get("name")->str, "alice");
    EXPECT_EQ(r2.get("name")->str, "bob");
}
