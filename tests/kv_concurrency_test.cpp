#include "storage/kv.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

// Reaches a KVTX's free list, so a test can check exactly which pages a write transaction may reuse.
struct KVTXTestPeer {
    static FreeList& free_list(KVTX& tx) {
        return tx.free_;
    }
};

namespace {
    constexpr auto kTimeout = std::chrono::seconds(10); // only a blocked thread takes this long

    using Bytes = std::optional<std::vector<uint8_t>>;

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

    // Waits for f. A thread blocked past kTimeout can't be joined, so the test binary stops instead of hanging.
    template <typename Future>
    void wait_or_abort(const Future& f) {
        if (f.wait_for(kTimeout) != std::future_status::ready) {
            std::fprintf(stderr, "a thread was still blocked after %lld s\n", static_cast<long long>(kTimeout.count()));
            std::abort();
        }
    }

    // Runs fn on its own thread; get() returns its result, rethrowing what it threw.
    template <typename T>
    class Async {
        public:
            template <typename Fn>
            explicit Async(Fn fn) : future_(promise_.get_future()) {
                thread_ = std::thread([this, fn = std::move(fn)]() mutable {
                    try {
                        promise_.set_value(fn());
                    } catch (...) {
                        promise_.set_exception(std::current_exception());
                    }
                });
            }
            Async(const Async&) = delete;
            Async& operator=(const Async&) = delete;

            ~Async() {
                if (future_.valid()) {
                    wait_or_abort(future_);
                }
                thread_.join();
            }

            T get() {
                wait_or_abort(future_);
                return future_.get();
            }

        private:
            std::promise<T> promise_;
            std::future<T> future_;
            std::thread thread_;
    };

    // Every key in tx holds its value from a single generation: "" with *generation set if so, else what's wrong.
    std::string check_snapshot(const KVReader& tx, int keys, int* generation) {
        std::optional<int> seen;
        int count = 0;
        for (BIter it = tx.seek(key(0), CMP_GE); it.valid(); it.next(), ++count) {
            auto [k, v] = it.deref();
            int g = std::stoi(std::string(v.begin(), v.end()));
            if (k != key(count) || v != value(count, g)) {
                return "bad pair at position " + std::to_string(count) + " of version " + std::to_string(tx.version);
            }
            if (seen && *seen != g) {
                return "generations " + std::to_string(*seen) + " and " + std::to_string(g) + " mixed in version " +
                       std::to_string(tx.version);
            }
            seen = g;
        }
        if (count != keys) {
            return std::to_string(count) + " keys in version " + std::to_string(tx.version);
        }
        *generation = *seen;
        return "";
    }

    class KVConcurrencyTest : public ::testing::Test {
    protected:
        void SetUp() override {
            const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
            path_ = (std::filesystem::temp_directory_path() /
                     (std::string("kv_concurrency_test_") + info->test_suite_name() + "_" + info->name() + ".db"))
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

        Bytes read_latest(const std::vector<uint8_t>& k) {
            KVReader tx;
            kv_.begin_read(&tx);
            Bytes val = tx.get(k);
            kv_.end_read(&tx);
            return val;
        }

        // Every page a write transaction beginning now may reuse, popped from its free list and dropped with it.
        std::vector<uint64_t> reusable_pages() {
            KVTX tx;
            kv_.begin(&tx);
            std::vector<uint64_t> pages;
            while (uint64_t ptr = KVTXTestPeer::free_list(tx).pop_head()) {
                pages.push_back(ptr);
            }
            kv_.abort(&tx);
            return pages;
        }

        std::string path_;
        KV kv_;
    };
}

TEST_F(KVConcurrencyTest, WhenAReaderBeginsBeforeAWriterCommitsThenItStillSeesThePreCommitDataAfterTheCommit) {
    commit_set(key(1), bytes("before"));
    std::promise<void> reader_began;
    std::future<void> began = reader_began.get_future();
    std::promise<void> writer_committed;
    std::shared_future<void> committed = writer_committed.get_future().share();

    Async<Bytes> reader([&] {
        KVReader tx;
        kv_.begin_read(&tx);
        reader_began.set_value();
        committed.wait();
        Bytes val = tx.get(key(1));
        kv_.end_read(&tx);
        return val;
    });
    Async<bool> writer([&] {
        began.wait();
        KVTX tx;
        kv_.begin(&tx);
        tx.set(key(1), bytes("after"));
        kv_.commit(&tx);
        writer_committed.set_value();
        return true;
    });

    EXPECT_TRUE(writer.get());
    EXPECT_EQ(reader.get(), bytes("before"));
    EXPECT_EQ(read_latest(key(1)), bytes("after"));
}

TEST_F(KVConcurrencyTest, WhenTwoReadersBeginAtDifferentVersionsThenEachSeesItsOwnSnapshot) {
    using Seen = std::pair<uint64_t, Bytes>;
    std::promise<void> first_began;
    std::promise<void> second_began;
    std::future<void> first_began_f = first_began.get_future();
    std::future<void> second_began_f = second_began.get_future();
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();

    // Begins a reader, which reads only once both readers are open and a newer version has been committed.
    auto reader_body = [&](std::promise<void>* began) {
        return [this, began, released]() -> Seen {
            KVReader tx;
            kv_.begin_read(&tx);
            began->set_value();
            released.wait();
            Seen seen{tx.version, tx.get(key(1))};
            kv_.end_read(&tx);
            return seen;
        };
    };

    commit_set(key(1), value(1, 1));
    Async<Seen> first(reader_body(&first_began));
    wait_or_abort(first_began_f);
    commit_set(key(1), value(1, 2));
    Async<Seen> second(reader_body(&second_began));
    wait_or_abort(second_began_f);
    commit_set(key(1), value(1, 3));
    release.set_value();

    EXPECT_EQ(first.get(), Seen(1, value(1, 1)));
    EXPECT_EQ(second.get(), Seen(2, value(1, 2)));
    EXPECT_EQ(read_latest(key(1)), value(1, 3));
}

TEST_F(KVConcurrencyTest, WhenAWriterCommitsWhileAReaderIsOpenThenTheCommitsDoNotWaitForTheReader) {
    commit_set(key(1), value(1, 1));
    KVReader reader;
    kv_.begin_read(&reader); // open on this thread for the whole test

    Async<int> writer([&] {
        int commits = 0;
        for (int generation = 2; generation <= 50; ++generation, ++commits) {
            commit_set(key(1), value(1, generation));
        }
        return commits;
    });
    EXPECT_EQ(writer.get(), 49); // stops the test binary instead if a commit blocks on the reader

    EXPECT_EQ(reader.get(key(1)), value(1, 1));
    kv_.end_read(&reader);
    EXPECT_EQ(read_latest(key(1)), value(1, 50));
}

// Checks the free list itself rather than the file size: the one page the reader's tree is made of must not be
// poppable while the reader is open, and must be once it ends.
TEST_F(KVConcurrencyTest, WhenAWriterFreesAPageAnOlderReaderCanSeeThenItIsNotReusedUntilTheReaderEnds) {
    commit_set(key(1), value(1, 1)); // a single leaf, which is also the root
    std::promise<uint64_t> reader_root;
    std::future<uint64_t> root = reader_root.get_future();
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();

    Async<Bytes> reader([&] {
        KVReader tx;
        kv_.begin_read(&tx);
        reader_root.set_value(tx.seek(key(1), CMP_GE).tree->root);
        released.wait();
        Bytes val = tx.get(key(1));
        kv_.end_read(&tx);
        return val;
    });
    wait_or_abort(root);
    const uint64_t pinned = root.get();

    commit_set(key(1), value(1, 2)); // replaces the leaf, freeing `pinned`
    std::vector<uint64_t> while_open = reusable_pages();
    EXPECT_EQ(std::count(while_open.begin(), while_open.end(), pinned), 0)
        << "page " << pinned << " was reusable while a reader could still reach it";

    KVTX write;
    kv_.begin(&write);
    write.set(key(1), value(1, 3));
    EXPECT_NE(write.seek(key(1), CMP_GE).tree->root, pinned); // the write had to take another page
    kv_.commit(&write);

    release.set_value();
    EXPECT_EQ(reader.get(), value(1, 1)); // returns once the reader has ended
    std::vector<uint64_t> after_end = reusable_pages();
    EXPECT_EQ(std::count(after_end.begin(), after_end.end(), pinned), 1);
}

TEST_F(KVConcurrencyTest, WhenManyReadersAreOpenAtOnceThenNoneWaitsForAnother) {
    constexpr int kReaders = 8;
    commit_set(key(1), value(1, 1));
    std::vector<std::promise<void>> began(kReaders);
    std::vector<std::future<void>> began_futures;
    for (std::promise<void>& p : began) {
        began_futures.push_back(p.get_future());
    }
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();

    std::vector<std::unique_ptr<Async<Bytes>>> readers;
    for (int i = 0; i < kReaders; ++i) {
        readers.push_back(std::make_unique<Async<Bytes>>([this, &began, released, i] {
            KVReader tx;
            kv_.begin_read(&tx);
            began[i].set_value();
            released.wait(); // stays open until every reader has begun
            Bytes val = tx.get(key(1));
            kv_.end_read(&tx);
            return val;
        }));
    }

    // If a reader had to wait for another to end, it couldn't begin before the release.
    bool all_began = true;
    for (const std::future<void>& f : began_futures) {
        all_began = all_began && f.wait_for(kTimeout) == std::future_status::ready;
    }
    release.set_value();
    EXPECT_TRUE(all_began) << "a reader couldn't begin while the others were open";
    for (const auto& reader : readers) {
        EXPECT_EQ(reader->get(), value(1, 1));
    }
}

// Readers keep taking snapshots, each held open across a few commits, while the writer rewrites every key generation
// after generation: every commit frees the previous generation's pages, which a reader still holding them must keep.
TEST_F(KVConcurrencyTest, WhenReadersRunThroughManyCommitsThenEverySnapshotHoldsOneWholeGeneration) {
    constexpr int kKeys = 50;
    constexpr int kGenerations = 200;
    constexpr int kReaders = 4;
    std::atomic<int> committed{0}; // the last generation the writer committed
    auto commit_generation = [&](int generation) {
        KVTX tx;
        kv_.begin(&tx);
        for (int i = 0; i < kKeys; ++i) {
            tx.set(key(i), value(i, generation));
        }
        kv_.commit(&tx);
        committed = generation;
    };
    commit_generation(0);

    std::atomic<bool> done{false};
    std::vector<std::unique_ptr<Async<std::string>>> readers;
    for (int r = 0; r < kReaders; ++r) {
        readers.push_back(std::make_unique<Async<std::string>>([&]() -> std::string {
            int last = 0;
            for (int snapshots = 0; snapshots == 0 || !done; ++snapshots) {
                KVReader tx;
                kv_.begin_read(&tx);
                // At least 2 more commits land before the read, and the 2nd would reuse this snapshot's pages.
                for (int start = committed; !done && committed < start + 3;) {
                    std::this_thread::yield();
                }
                int generation = 0;
                std::string err = check_snapshot(tx, kKeys, &generation);
                kv_.end_read(&tx);
                if (!err.empty()) {
                    return err;
                }
                if (generation < last) {
                    return "generation " + std::to_string(generation) + " after " + std::to_string(last);
                }
                last = generation;
            }
            return "";
        }));
    }

    for (int generation = 1; generation <= kGenerations; ++generation) {
        commit_generation(generation);
    }
    done = true;

    for (const auto& reader : readers) {
        EXPECT_EQ(reader->get(), "");
    }
    EXPECT_EQ(read_latest(key(kKeys - 1)), value(kKeys - 1, kGenerations));
}
