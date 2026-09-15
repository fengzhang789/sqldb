#include "access/row_codec.h"

#include "encoding/order_preserving.h"
#include "catalog/tabledef.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {
    constexpr int64_t I64_MIN = std::numeric_limits<int64_t>::min();
    constexpr int64_t I64_MAX = std::numeric_limits<int64_t>::max();

    // (name, age, id) gives encode_key_partial a bytes, an int64 and a pk column to pad.
    const std::vector<std::string> NAME_AGE_ID = {"name", "age", "id"};

    TableDef make_people_table() {
        return TableDefBuilder("people")
            .add_col("id", INT_64)
            .add_col("name", BYTES)
            .add_col("age", INT_64)
            .add_col("bio", BYTES)
            .set_pkeys(1)
            .set_prefix(5)
            .build();
    }

    // Compares row's first bound.size() values to bound: int64s numerically, bytes as unsigned strings.
    int compare_prefix(const std::vector<Value>& row, const std::vector<Value>& bound) {
        for (size_t i = 0; i < bound.size(); ++i) {
            int c = row[i].type == INT_64 ? (row[i].int64 > bound[i].int64) - (row[i].int64 < bound[i].int64)
                                          : row[i].str.compare(bound[i].str);
            if (c != 0) {
                return c;
            }
        }
        return 0;
    }

    TableDef make_users_table() {
        return TableDefBuilder("users")
            .add_col("id", INT_64)
            .add_col("name", BYTES)
            .add_col("age", INT_64)
            .set_pkeys(1)
            .set_prefix(5)
            .build();
    }
}

// ============================================================================
// check_record
// ============================================================================
TEST(CheckRecordTest, WhenColumnsAreOutOfOrderThenCheckRecordReordersThem) {
    TableDef def = make_users_table();
    Record rec;
    rec.add_int64("age", 30).add_str("name", "alice").add_int64("id", 1);

    std::vector<Value> out;
    std::string err;
    ASSERT_TRUE(check_record(def, rec, static_cast<int>(def.cols.size()), &out, &err)) << err;

    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].int64, 1);
    EXPECT_EQ(out[1].str, "alice");
    EXPECT_EQ(out[2].int64, 30);
}

TEST(CheckRecordTest, WhenNEqualsPkeysThenCheckRecordAcceptsOnlyThePrimaryKeyColumns) {
    TableDef def = make_users_table();
    Record rec;
    rec.add_int64("id", 42);

    std::vector<Value> out;
    std::string err;
    ASSERT_TRUE(check_record(def, rec, def.pkeys, &out, &err)) << err;
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].int64, 42);
}

TEST(CheckRecordTest, WhenAColumnIsMissingThenCheckRecordFails) {
    TableDef def = make_users_table();
    Record rec;
    rec.add_int64("id", 1).add_str("name", "alice");  // missing "age"

    std::vector<Value> out;
    std::string err;
    EXPECT_FALSE(check_record(def, rec, static_cast<int>(def.cols.size()), &out, &err));
    EXPECT_FALSE(err.empty());
}

TEST(CheckRecordTest, WhenAnExtraColumnIsPresentThenCheckRecordFails) {
    TableDef def = make_users_table();
    Record rec;
    rec.add_int64("id", 1);
    rec.add_str("extra", "nope");

    std::vector<Value> out;
    std::string err;
    EXPECT_FALSE(check_record(def, rec, def.pkeys, &out, &err));
    EXPECT_FALSE(err.empty());
}

TEST(CheckRecordTest, WhenAColumnTypeMismatchesThenCheckRecordFails) {
    TableDef def = make_users_table();
    Record rec;
    rec.add_str("id", "not-an-int");  // id is INT_64

    std::vector<Value> out;
    std::string err;
    EXPECT_FALSE(check_record(def, rec, def.pkeys, &out, &err));
    EXPECT_FALSE(err.empty());
}

// ============================================================================
// encode_key / encode_values / decode_values
// ============================================================================
TEST(RowCodecTest, WhenEncodeKeyIsCalledThenItStartsWithTheBigEndianPrefix) {
    std::vector<Value> pk = {Value::make_int64(7)};
    std::string key = encode_key(0x01020304, pk);

    ASSERT_GE(key.size(), 4u);
    EXPECT_EQ(static_cast<uint8_t>(key[0]), 0x01);
    EXPECT_EQ(static_cast<uint8_t>(key[1]), 0x02);
    EXPECT_EQ(static_cast<uint8_t>(key[2]), 0x03);
    EXPECT_EQ(static_cast<uint8_t>(key[3]), 0x04);
}

TEST(RowCodecTest, WhenPrimaryKeyValuesDifferThenEncodeKeyDiffers) {
    std::vector<Value> a = {Value::make_int64(1)};
    std::vector<Value> b = {Value::make_int64(2)};
    EXPECT_NE(encode_key(5, a), encode_key(5, b));
}

TEST(RowCodecTest, WhenValuesAreEncodedThenDecodeValuesRoundTrips) {
    std::vector<Value> values = {Value::make_bytes("alice"), Value::make_int64(30)};
    std::string data = encode_values(values);

    std::vector<Value> out(2);
    out[0].type = BYTES;
    out[1].type = INT_64;
    decode_values(data, &out);

    EXPECT_EQ(out[0].str, "alice");
    EXPECT_EQ(out[1].int64, 30);
}

TEST(RowCodecTest, WhenAnInt64IsNegativeThenEncodeAndDecodeValuesRoundTrip) {
    std::vector<Value> values = {Value::make_int64(-12345)};
    std::string data = encode_values(values);

    std::vector<Value> out(1);
    out[0].type = INT_64;
    decode_values(data, &out);

    EXPECT_EQ(out[0].int64, -12345);
}

TEST(RowCodecTest, WhenBytesAreEmptyThenEncodeAndDecodeValuesRoundTrip) {
    std::vector<Value> values = {Value::make_bytes("")};
    std::string data = encode_values(values);

    std::vector<Value> out(1);
    out[0].type = BYTES;
    decode_values(data, &out);

    EXPECT_EQ(out[0].str, "");
}

// ============================================================================
// decode_row
// ============================================================================
TEST(DecodeRowTest, WhenAPrimaryKeyPairIsDecodedThenDecodeRowReplacesTheRecordWithEveryColumnInOrder) {
    TableDef def = make_users_table();
    std::string key = encode_key(def.prefix, {Value::make_int64(7)});
    std::string val = encode_values({Value::make_bytes("alice"), Value::make_int64(30)});

    Record rec;
    rec.add_str("stale", "x");
    decode_row(def, key, val, &rec);

    EXPECT_EQ(rec.cols, def.cols);
    ASSERT_EQ(rec.vals.size(), 3u);
    EXPECT_TRUE(rec.vals[0] == Value::make_int64(7));
    EXPECT_TRUE(rec.vals[1] == Value::make_bytes("alice"));
    EXPECT_TRUE(rec.vals[2] == Value::make_int64(30));
}

// ============================================================================
// encode_key_partial
// ============================================================================
TEST(EncodeKeyPartialTest, WhenCmpIsGeOrLtThenEncodeKeyPartialAddsNoPadding) {
    TableDef def = make_people_table();
    std::vector<Value> name = {Value::make_bytes("bob")};
    for (CMP cmp : {CMP_GE, CMP_LT}) {
        EXPECT_EQ(encode_key_partial(5, name, def, NAME_AGE_ID, cmp), encode_key(5, name));
        EXPECT_EQ(encode_key_partial(5, {}, def, NAME_AGE_ID, cmp), encode_key(5, {}));
    }
}

TEST(EncodeKeyPartialTest, WhenEveryIndexColumnHasAValueThenEncodeKeyPartialEqualsEncodeKey) {
    TableDef def = make_people_table();
    std::vector<Value> full = {Value::make_bytes("bob"), Value::make_int64(30), Value::make_int64(7)};
    for (CMP cmp : {CMP_GE, CMP_GT, CMP_LT, CMP_LE}) {
        EXPECT_EQ(encode_key_partial(5, full, def, NAME_AGE_ID, cmp), encode_key(5, full));
    }
}

TEST(EncodeKeyPartialTest, WhenMissingColumnsAreInt64ThenGtAndLePadEachWithEightFfBytes) {
    TableDef def = make_people_table();
    const std::vector<std::string> age_id = {"age", "id"};
    std::vector<Value> age = {Value::make_int64(30)};
    for (CMP cmp : {CMP_GT, CMP_LE}) {
        EXPECT_EQ(encode_key_partial(5, age, def, age_id, cmp), encode_key(5, age) + std::string(8, '\xff'));
        EXPECT_EQ(encode_key_partial(5, {}, def, age_id, cmp), encode_key(5, {}) + std::string(16, '\xff'));
    }
}

TEST(EncodeKeyPartialTest, WhenAMissingColumnIsBytesThenGtAndLePadASingleFfAndStop) {
    TableDef def = make_people_table();
    const std::vector<std::string> age_name_id = {"age", "name", "id"};
    for (CMP cmp : {CMP_GT, CMP_LE}) {
        EXPECT_EQ(encode_key_partial(5, {}, def, NAME_AGE_ID, cmp), encode_key(5, {}) + "\xff");
        EXPECT_EQ(encode_key_partial(5, {}, def, age_name_id, cmp), encode_key(5, {}) + std::string(9, '\xff'));
    }
}

TEST(EncodeKeyPartialTest, WhenABoundCoversAPrefixOfTheIndexThenEveryKeySortsAgainstItLikeItsPrefixDoes) {
    TableDef def = make_people_table();
    const std::string names[] = {"", "a", std::string("a\0", 2), "a\xff", "b", "\xfe", "\xff", "\xff\xff"};
    const int64_t ages[] = {I64_MIN, -1, 0, I64_MAX};
    const int64_t ids[] = {I64_MIN, 0, I64_MAX};

    std::vector<std::vector<Value>> rows;
    for (const std::string& name : names) {
        for (int64_t age : ages) {
            for (int64_t id : ids) {
                rows.push_back({Value::make_bytes(name), Value::make_int64(age), Value::make_int64(id)});
            }
        }
    }

    for (const auto& bound_row : rows) {
        for (size_t n = 0; n <= bound_row.size(); ++n) {
            std::vector<Value> bound(bound_row.begin(), bound_row.begin() + n);
            std::string low = encode_key_partial(5, bound, def, NAME_AGE_ID, CMP_GE);
            std::string high = encode_key_partial(5, bound, def, NAME_AGE_ID, CMP_LE);
            ASSERT_EQ(encode_key_partial(5, bound, def, NAME_AGE_ID, CMP_LT), low);
            ASSERT_EQ(encode_key_partial(5, bound, def, NAME_AGE_ID, CMP_GT), high);

            for (const auto& row : rows) {
                int c = compare_prefix(row, bound);
                std::string key = encode_key(5, row);
                ASSERT_EQ(key >= low, c >= 0) << "bound columns " << n;
                ASSERT_EQ(key <= high, c <= 0) << "bound columns " << n;
            }
        }
    }
}
