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
    {
        std::ofstream f(path_, std::ios::binary);
        f << "preexisting";
    }

    KV db(path_);
    EXPECT_NO_THROW(db.open());
    // The existing file must not be truncated by opening it.
    EXPECT_EQ(std::filesystem::file_size(path_), 11u);
}

TEST_F(KVTest, OpenThrowsWhenParentDirectoryDoesNotExist) {
    KV db((std::filesystem::path(path_).parent_path() / "no_such_dir" / "kv.db").string());
    EXPECT_THROW(db.open(), std::runtime_error);
}

// TODO: re-add set/get/del persistence and signature-validation tests once
// open()/write_pages()/update_root() are implemented.
