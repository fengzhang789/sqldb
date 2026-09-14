#include "access/row_codec.h"

#include "encoding/order_preserving.h"
#include "catalog/tabledef.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {
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
