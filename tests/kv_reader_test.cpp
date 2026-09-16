#include "storage/kv_reader.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "storage/kv.h"

namespace {
    std::vector<uint8_t> bytes(const std::string& s) {
        return std::vector<uint8_t>(s.begin(), s.end());
    }

    // Zero-padded, so keys sort in numeric order.
    std::vector<uint8_t> key(int i) {
        std::string digits = std::to_string(i);
        return bytes("key" + std::string(4 - digits.size(), '0') + digits);
    }

    std::vector<uint8_t> value(int i, int generation) {
        return bytes(std::to_string(generation) + ":" + std::to_string(i) + std::string(100, 'x'));
    }

    uintmax_t file_pages(const std::string& path) {
        return std::filesystem::file_size(path) / BTREE_PAGE_SIZE;
    }

    class KVReaderTest : public ::testing::Test {
    protected:
        void SetUp() override {
            const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
            path_ = (std::filesystem::temp_directory_path() /
                     (std::string("kv_reader_test_") + info->test_suite_name() + "_" + info->name() + ".db"))
                        .string();
            std::filesystem::remove(path_);
            kv_.path = path_;
            kv_.open();
        }

        void TearDown() override {
            kv_.close();
            std::filesystem::remove(path_);
        }

        void commit_set(const std::vector<uint8_t>& k, const std::vector<uint8_t>& v) {
            KVTX tx;
            kv_.begin(&tx);
            tx.set(k, v);
            kv_.commit(&tx);
        }

        // One commit setting each of the first `keys` keys to its value in `generation`.
        void commit_generation(int keys, int generation) {
            KVTX tx;
            kv_.begin(&tx);
            for (int i = 0; i < keys; ++i) {
                tx.set(key(i), value(i, generation));
            }
            kv_.commit(&tx);
        }

        std::string path_;
        KV kv_;
    };
}

// ============================================================================
// Snapshots
// ============================================================================
TEST_F(KVReaderTest, WhenNothingIsCommittedThenAReaderFindsNothing) {
    KVReader reader;
    kv_.begin_read(&reader);

    EXPECT_EQ(reader.version, 0u);
    EXPECT_EQ(reader.get(key(1)), std::nullopt);
    EXPECT_FALSE(reader.seek(key(1), CMP_GE).valid());
    kv_.end_read(&reader);
}

TEST_F(KVReaderTest, WhenAWriteCommitsAfterAReaderBeganThenTheReaderStillGetsTheOldValue) {
    commit_set(key(1), bytes("old"));
    KVReader reader;
    kv_.begin_read(&reader);

    commit_set(key(1), bytes("new"));
    commit_set(key(2), bytes("added"));

    EXPECT_EQ(reader.get(key(1)), bytes("old"));
    EXPECT_EQ(reader.get(key(2)), std::nullopt);
    kv_.end_read(&reader);

    KVReader later;
    kv_.begin_read(&later);
    EXPECT_EQ(later.get(key(1)), bytes("new"));
    EXPECT_EQ(later.get(key(2)), bytes("added"));
    kv_.end_read(&later);
}

TEST_F(KVReaderTest, WhenReadersBeginAtDifferentVersionsThenEachSeesItsOwnSnapshot) {
    std::vector<std::unique_ptr<KVReader>> readers;
    for (int generation = 0; generation < 5; ++generation) {
        commit_set(key(0), value(0, generation));
        readers.push_back(std::make_unique<KVReader>());
        kv_.begin_read(readers.back().get());
    }

    for (int generation = 0; generation < 5; ++generation) {
        EXPECT_EQ(readers[generation]->version, static_cast<uint64_t>(generation + 1));
        EXPECT_EQ(readers[generation]->get(key(0)), value(0, generation));
    }
    for (const auto& reader : readers) {
        kv_.end_read(reader.get());
    }
}

TEST_F(KVReaderTest, WhenAReaderSeeksThenItWalksOnlyTheKeysOfItsSnapshot) {
    for (int i = 0; i < 10; i += 2) {
        commit_set(key(i), value(i, 0));
    }
    KVReader reader;
    kv_.begin_read(&reader);

    KVTX tx;
    kv_.begin(&tx);
    EXPECT_TRUE(tx.del(key(4)));
    tx.set(key(5), value(5, 0));
    kv_.commit(&tx);

    std::vector<std::vector<uint8_t>> keys;
    for (BIter it = reader.seek(key(0), CMP_GE); it.valid(); it.next()) {
        keys.push_back(it.deref().first);
    }
    EXPECT_EQ(keys, (std::vector<std::vector<uint8_t>>{key(0), key(2), key(4), key(6), key(8)}));
    kv_.end_read(&reader);
}

TEST_F(KVReaderTest, WhenAWriteTransactionIsOpenThenAReaderBeginsOnTheLastCommitWithoutItsWrites) {
    commit_set(key(1), bytes("committed"));
    KVTX tx;
    kv_.begin(&tx);
    tx.set(key(1), bytes("pending"));
    tx.set(key(2), bytes("pending"));

    KVReader reader;
    kv_.begin_read(&reader);
    EXPECT_EQ(reader.get(key(1)), bytes("committed"));
    EXPECT_EQ(reader.get(key(2)), std::nullopt);

    kv_.commit(&tx);
    EXPECT_EQ(reader.get(key(1)), bytes("committed"));
    kv_.end_read(&reader);
}

TEST_F(KVReaderTest, WhenAWriteTransactionReadsAsAKVReaderThenItSeesItsOwnWrites) {
    commit_set(key(1), bytes("committed"));
    KVTX tx;
    kv_.begin(&tx);
    tx.set(key(1), bytes("mine"));

    const KVReader& as_reader = tx;
    EXPECT_EQ(as_reader.get(key(1)), bytes("mine"));
    BIter it = as_reader.seek(key(1), CMP_GE);
    EXPECT_TRUE(it.valid() && it.deref().second == bytes("mine"));
    kv_.abort(&tx);
}

// ============================================================================
// Page reuse
// ============================================================================
// Without the reader, each generation would reuse the pages the one before it freed, overwriting the reader's tree.
TEST_F(KVReaderTest, WhenAReaderStaysOpenAcrossManyCommitsThenEveryPageItReachesStaysIntact) {
    constexpr int kKeys = 100;
    commit_generation(kKeys, 0);
    KVReader reader;
    kv_.begin_read(&reader);

    for (int generation = 1; generation <= 30; ++generation) {
        commit_generation(kKeys, generation);
    }

    int i = 0;
    for (BIter it = reader.seek(key(0), CMP_GE); it.valid(); it.next(), ++i) {
        auto [k, v] = it.deref();
        EXPECT_EQ(k, key(i));
        EXPECT_EQ(v, value(i, 0)) << "key " << i;
    }
    EXPECT_EQ(i, kKeys);
    kv_.end_read(&reader);
}

TEST_F(KVReaderTest, WhenAReaderEndsThenLaterCommitsReuseThePagesItKeptAlive) {
    constexpr int kKeys = 100;
    commit_generation(kKeys, 0);
    commit_generation(kKeys, 1);
    const uintmax_t before = file_pages(path_);

    KVReader reader;
    kv_.begin_read(&reader);
    for (int generation = 2; generation <= 11; ++generation) {
        commit_generation(kKeys, generation);
    }
    const uintmax_t pinned = file_pages(path_);
    kv_.end_read(&reader);
    EXPECT_GT(pinned, before + 10) << "the commits reused pages the open reader could reach";

    for (int generation = 12; generation <= 40; ++generation) {
        commit_generation(kKeys, generation);
    }
    EXPECT_LE(file_pages(path_), pinned + 1) << "the pages freed while the reader was open were not reused";
}

TEST_F(KVReaderTest, WhenAPageIsOutsideTheSnapshotsMmapThenPageGetMappedThrows) {
    KVReader empty;
    kv_.begin_read(&empty);
    EXPECT_THROW(empty.page_get_mapped(2), std::out_of_range); // nothing is mapped before the 1st commit
    kv_.end_read(&empty);

    commit_set(key(1), value(1, 0));
    KVReader reader;
    kv_.begin_read(&reader);
    EXPECT_NO_THROW(reader.page_get_mapped(2));
    EXPECT_THROW(reader.page_get_mapped(uint64_t{1} << 40), std::out_of_range);
    kv_.end_read(&reader);
}
