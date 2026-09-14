#include "btree_iter.h"
#include "pagemanager.h"

#include <algorithm>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

namespace {
    using Bytes = std::vector<uint8_t>;

    Bytes bytes(const std::string& s) {
        return Bytes(s.begin(), s.end());
    }

    class InMemoryPageManager : public IPageManager {
    public:
        BNode get(uint64_t ptr) const override { return pages_.at(ptr); }

        uint64_t new_page(const BNode& node) override {
            uint64_t ptr = next_ptr_++;
            pages_[ptr] = node;
            return ptr;
        }

        void del(uint64_t ptr) override { pages_.erase(ptr); }

    private:
        std::unordered_map<uint64_t, BNode> pages_;
        uint64_t next_ptr_ = 1;
    };

    // A key that sorts by `idx`: its 4-byte big-endian index padded with filler out to `len` bytes.
    Bytes indexed_key(uint32_t idx, size_t len) {
        Bytes k(len, 'x');
        k[0] = static_cast<uint8_t>(idx >> 24);
        k[1] = static_cast<uint8_t>(idx >> 16);
        k[2] = static_cast<uint8_t>(idx >> 8);
        k[3] = static_cast<uint8_t>(idx);
        return k;
    }

    uint32_t index_of(const Bytes& key) {
        return (static_cast<uint32_t>(key[0]) << 24) | (static_cast<uint32_t>(key[1]) << 16) |
               (static_cast<uint32_t>(key[2]) << 8) | key[3];
    }

    Bytes val_for(uint32_t idx) {
        return bytes("v" + std::to_string(idx));
    }

    struct TreeShape {
        std::string name;
        uint32_t nkeys; // inserted at even indices, leaving odd indices free as between-key probes
        size_t key_len; // longer keys fit fewer per node, so the tree grows deeper
        bool delete_half; // then delete a random half of the keys, exercising merges
        size_t depth; // expected height, confirming the shape
    };

    const TreeShape TREE_SHAPES[] = {
        {"SingleLeaf", 5, 8, false, 1},
        {"TwoLevels", 60, 100, false, 2},
        {"ThreeLevels", 30, 900, false, 3},
        {"FourLevels", 80, 900, false, 4},
        {"FourLevelsAfterDeletes", 150, 900, true, 4},
    };

    std::string shape_name(const ::testing::TestParamInfo<TreeShape>& info) {
        return info.param.name;
    }

    // Names the shape in failure output and CTest test names instead of dumping its bytes.
    void PrintTo(const TreeShape& shape, std::ostream* os) {
        *os << shape.name;
    }

    // Builds a BTree of GetParam()'s shape from shuffled inserts, alongside the sorted indices of its keys.
    class BIterTest : public ::testing::TestWithParam<TreeShape> {
    protected:
        void SetUp() override {
            const TreeShape& shape = GetParam();
            for (uint32_t i = 0; i < shape.nkeys; ++i) {
                idxs_.push_back(2 * i);
            }
            std::mt19937 rng(42);
            std::shuffle(idxs_.begin(), idxs_.end(), rng);
            for (uint32_t idx : idxs_) {
                tree_.insert(key(idx), val_for(idx));
            }
            if (shape.delete_half) {
                size_t half = idxs_.size() / 2;
                for (size_t i = 0; i < half; ++i) {
                    ASSERT_TRUE(tree_.remove(key(idxs_[i])));
                }
                idxs_.erase(idxs_.begin(), idxs_.begin() + static_cast<std::ptrdiff_t>(half));
            }
            std::sort(idxs_.begin(), idxs_.end());
            ASSERT_EQ(tree_.seek_le({}).path.size(), shape.depth);
        }

        Bytes key(uint32_t idx) const {
            return indexed_key(idx, GetParam().key_len);
        }

        // Expects `iter` on the KV pair with index `idx`, or invalid if there is none.
        void expect_at(const BIter& iter, std::optional<uint32_t> idx) const {
            if (!idx.has_value()) {
                EXPECT_FALSE(iter.valid());
                return;
            }
            ASSERT_TRUE(iter.valid()) << "expected index " << *idx;
            auto [k, v] = iter.deref();
            ASSERT_EQ(index_of(k), *idx);
            EXPECT_TRUE(k == key(*idx));
            EXPECT_EQ(v, val_for(*idx));
        }

        // Records the current key's index after checking its KV pair is intact.
        void visit(const BIter& iter, std::vector<uint32_t>& visited) const {
            uint32_t idx = index_of(iter.deref().first);
            ASSERT_NO_FATAL_FAILURE(expect_at(iter, idx));
            visited.push_back(idx);
        }

        InMemoryPageManager pages_;
        BTree tree_{0, &pages_};
        std::vector<uint32_t> idxs_; // sorted indices of every key in tree_
    };

    class BTreeSeekTest : public BIterTest {};
}

// ============================================================================
// cmp_ok
// ============================================================================
TEST(CmpOkTest, WhenKeyIsComparedWithRefThenEachCmpMatchesItsRelation) {
    const Bytes ref = bytes("b");
    const Bytes below = {'a', 0xff}; // bytes compare unsigned
    const Bytes above = {'b', 0x00}; // a longer key sorts after its prefix
    struct Case {
        CMP cmp;
        bool below, equal, above;
    };
    const Case cases[] = {
        {CMP_GE, false, true, true},
        {CMP_GT, false, false, true},
        {CMP_LT, true, false, false},
        {CMP_LE, true, true, false},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE("cmp " + std::to_string(c.cmp));
        EXPECT_EQ(cmp_ok(below, c.cmp, ref), c.below);
        EXPECT_EQ(cmp_ok(ref, c.cmp, ref), c.equal);
        EXPECT_EQ(cmp_ok(above, c.cmp, ref), c.above);
    }
}

// ============================================================================
// BIter: empty trees
// ============================================================================
TEST(BIterEmptyTreeTest, WhenTreeIsEmptyThenIteratorIsInvalidAndStepsAreNoOps) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};

    BIter iter = tree.seek_le(bytes("k"));
    EXPECT_TRUE(iter.path.empty());
    EXPECT_FALSE(iter.valid());
    iter.next();
    EXPECT_FALSE(iter.valid());
    iter.prev();
    EXPECT_FALSE(iter.valid());

    for (CMP cmp : {CMP_GE, CMP_GT, CMP_LT, CMP_LE}) {
        EXPECT_FALSE(tree.seek(bytes("k"), cmp).valid()) << "cmp " << cmp;
    }
}

TEST(BIterEmptyTreeTest, WhenOnlyTheSentinelRemainsThenIteratorIsInvalidInBothDirections) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};
    tree.insert(bytes("k"), bytes("v"));
    ASSERT_TRUE(tree.remove(bytes("k")));

    BIter iter = tree.seek_le(bytes("k"));
    EXPECT_FALSE(iter.valid()); // on the sentinel
    iter.next();
    EXPECT_FALSE(iter.valid()); // after the last key
    iter.prev();
    EXPECT_FALSE(iter.valid()); // back on the sentinel
    iter.prev();
    EXPECT_FALSE(iter.valid());

    for (CMP cmp : {CMP_GE, CMP_GT, CMP_LT, CMP_LE}) {
        EXPECT_FALSE(tree.seek(bytes("k"), cmp).valid()) << "cmp " << cmp;
    }
}

TEST(BIterDerefTest, WhenIteratorIsInvalidThenDerefAsserts) {
    InMemoryPageManager pages;
    BTree tree{0, &pages};
    tree.insert(bytes("k"), bytes("v"));

    BIter iter = tree.seek_le({});
    ASSERT_FALSE(iter.valid());
    EXPECT_DEATH(iter.deref(), "");
}

// ============================================================================
// BIter: walks over trees of varying depth
// ============================================================================
TEST_P(BIterTest, WhenWalkingForwardFromBeforeTheFirstKeyThenEveryKeyIsVisitedInOrder) {
    BIter iter = tree_.seek_le({});
    ASSERT_FALSE(iter.valid());

    std::vector<uint32_t> visited;
    for (iter.next(); iter.valid(); iter.next()) {
        ASSERT_NO_FATAL_FAILURE(visit(iter, visited));
    }
    EXPECT_EQ(visited, idxs_);
}

TEST_P(BIterTest, WhenWalkingBackwardFromTheLastKeyThenEveryKeyIsVisitedInReverseOrder) {
    BIter iter = tree_.seek_le(Bytes(BTREE_MAX_KEY_SIZE, 0xff));

    std::vector<uint32_t> visited;
    for (; iter.valid(); iter.prev()) {
        ASSERT_NO_FATAL_FAILURE(visit(iter, visited));
    }
    std::reverse(visited.begin(), visited.end());
    EXPECT_EQ(visited, idxs_);
}

TEST_P(BIterTest, WhenSteppingPastTheLastKeyThenValidIsFalseUntilPrevReturnsToIt) {
    BIter iter = tree_.seek_le(key(idxs_.back()));
    ASSERT_NO_FATAL_FAILURE(expect_at(iter, idxs_.back()));

    iter.next();
    EXPECT_FALSE(iter.valid());
    iter.next(); // further steps stay past the end
    EXPECT_FALSE(iter.valid());

    iter.prev();
    expect_at(iter, idxs_.back());
}

TEST_P(BIterTest, WhenSteppingBeforeTheFirstKeyThenValidIsFalseUntilNextReturnsToIt) {
    BIter iter = tree_.seek_le(key(idxs_.front()));
    ASSERT_NO_FATAL_FAILURE(expect_at(iter, idxs_.front()));

    iter.prev();
    EXPECT_FALSE(iter.valid());
    iter.prev(); // further steps stay before the start
    EXPECT_FALSE(iter.valid());

    iter.next();
    expect_at(iter, idxs_.front());
}

TEST_P(BIterTest, WhenAlternatingNextAndPrevThenEachPairOfStepsReturnsToTheSameKey) {
    BIter iter = tree_.seek_le(key(idxs_.front()));
    for (uint32_t idx : idxs_) {
        ASSERT_NO_FATAL_FAILURE(expect_at(iter, idx));
        iter.next();
        iter.prev();
        ASSERT_NO_FATAL_FAILURE(expect_at(iter, idx));
        iter.next();
    }
    EXPECT_FALSE(iter.valid());
}

// ============================================================================
// BTree::seek: each CMP against exact, between, and out-of-range probes
// ============================================================================
TEST_P(BTreeSeekTest, WhenProbeMatchesAKeyThenInclusiveCmpsStayAndExclusiveCmpsStepToTheNeighbour) {
    for (size_t i = 0; i < idxs_.size(); ++i) {
        SCOPED_TRACE("probe index " + std::to_string(idxs_[i]));
        std::optional<uint32_t> before = i > 0 ? std::optional<uint32_t>(idxs_[i - 1]) : std::nullopt;
        std::optional<uint32_t> after = i + 1 < idxs_.size() ? std::optional<uint32_t>(idxs_[i + 1]) : std::nullopt;
        Bytes probe = key(idxs_[i]);
        expect_at(tree_.seek(probe, CMP_GE), idxs_[i]);
        expect_at(tree_.seek(probe, CMP_LE), idxs_[i]);
        expect_at(tree_.seek(probe, CMP_GT), after);
        expect_at(tree_.seek(probe, CMP_LT), before);
    }
}

TEST_P(BTreeSeekTest, WhenProbeFallsBetweenTwoKeysThenEachCmpLandsOnTheNeighbourInItsDirection) {
    for (size_t i = 0; i + 1 < idxs_.size(); ++i) {
        SCOPED_TRACE("probe index " + std::to_string(idxs_[i] + 1));
        Bytes probe = key(idxs_[i] + 1); // odd indices are never inserted
        expect_at(tree_.seek(probe, CMP_GE), idxs_[i + 1]);
        expect_at(tree_.seek(probe, CMP_GT), idxs_[i + 1]);
        expect_at(tree_.seek(probe, CMP_LE), idxs_[i]);
        expect_at(tree_.seek(probe, CMP_LT), idxs_[i]);
    }
}

TEST_P(BTreeSeekTest, WhenProbePrecedesEveryKeyThenGreaterCmpsLandOnTheFirstKeyAndLesserCmpsAreInvalid) {
    for (const Bytes& probe : {Bytes{}, Bytes{0x00}}) { // the sentinel's own key, and one just after it
        SCOPED_TRACE("probe size " + std::to_string(probe.size()));
        expect_at(tree_.seek(probe, CMP_GE), idxs_.front());
        expect_at(tree_.seek(probe, CMP_GT), idxs_.front());
        expect_at(tree_.seek(probe, CMP_LE), std::nullopt);
        expect_at(tree_.seek(probe, CMP_LT), std::nullopt);
    }
}

TEST_P(BTreeSeekTest, WhenProbeFollowsEveryKeyThenLesserCmpsLandOnTheLastKeyAndGreaterCmpsAreInvalid) {
    for (const Bytes& probe : {key(idxs_.back() + 1), Bytes(BTREE_MAX_KEY_SIZE, 0xff)}) {
        SCOPED_TRACE("probe size " + std::to_string(probe.size()));
        expect_at(tree_.seek(probe, CMP_LE), idxs_.back());
        expect_at(tree_.seek(probe, CMP_LT), idxs_.back());
        expect_at(tree_.seek(probe, CMP_GE), std::nullopt);
        expect_at(tree_.seek(probe, CMP_GT), std::nullopt);
    }
}

INSTANTIATE_TEST_SUITE_P(TreeShapes, BIterTest, ::testing::ValuesIn(TREE_SHAPES), shape_name);
INSTANTIATE_TEST_SUITE_P(TreeShapes, BTreeSeekTest, ::testing::ValuesIn(TREE_SHAPES), shape_name);
