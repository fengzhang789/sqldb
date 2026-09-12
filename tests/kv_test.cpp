#include "kv.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {
    std::vector<uint8_t> bytes(const std::string& s) {
        return std::vector<uint8_t>(s.begin(), s.end());
    }

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
        std::string garbage(32, '\xff');
        f.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
    }

    KV db(path_);
    EXPECT_THROW(db.open(), std::runtime_error);
}

TEST_F(KVTest, OpenThrowsWhenFileIsTooSmallForAMetaPage) {
    {
        std::ofstream f(path_, std::ios::binary);
        f << "DB"; // shorter than the 32-byte meta page
    }

    KV db(path_);
    EXPECT_THROW(db.open(), std::runtime_error);
}
