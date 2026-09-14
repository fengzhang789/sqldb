#include "encoding/order_preserving.h"

#include <cstdint>
#include <cstdio>
#include <iterator>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {
    constexpr int64_t I64_MIN = std::numeric_limits<int64_t>::min();
    constexpr int64_t I64_MAX = std::numeric_limits<int64_t>::max();

    std::string enc_int64(int64_t v) {
        std::string out;
        encode_int64(&out, v);
        return out;
    }

    std::string enc_bytes(const std::string& s) {
        std::string out;
        encode_bytes(&out, s);
        return out;
    }

    int sign(int x) {
        return (x > 0) - (x < 0);
    }

    // Unsigned lexicographic byte order, written out so the oracle doesn't lean on std::string's comparison.
    int compare_bytes(const std::string& a, const std::string& b) {
        for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
            uint8_t x = static_cast<uint8_t>(a[i]);
            uint8_t y = static_cast<uint8_t>(b[i]);
            if (x != y) {
                return x < y ? -1 : 1;
            }
        }
        return (a.size() > b.size()) - (a.size() < b.size());
    }

    // Reference order for rows of one schema: column by column, int64s numerically and bytes lexicographically.
    int logical_compare(const std::vector<Value>& a, const std::vector<Value>& b) {
        for (size_t i = 0; i < a.size(); ++i) {
            int c = a[i].type == INT_64 ? (a[i].int64 > b[i].int64) - (a[i].int64 < b[i].int64)
                                        : compare_bytes(a[i].str, b[i].str);
            if (c != 0) {
                return c;
            }
        }
        return 0;
    }

    std::string describe(const std::vector<Value>& row) {
        std::string out = "(";
        for (const Value& v : row) {
            if (v.type == INT_64) {
                out += std::to_string(v.int64);
            } else {
                out += '"';
                for (unsigned char c : v.str) {
                    char buf[5];
                    std::snprintf(buf, sizeof(buf), "\\x%02x", c);
                    out += buf;
                }
                out += '"';
            }
            out += ", ";
        }
        return out + ")";
    }

    // Biased toward boundaries and ties, where ordering bugs hide.
    int64_t random_int64(std::mt19937_64& rng) {
        const int64_t edges[] = {I64_MIN, I64_MIN + 1, -256, -1, 0, 1, 255, I64_MAX - 1, I64_MAX};
        switch (rng() % 3) {
            case 0: return edges[rng() % std::size(edges)];
            case 1: return static_cast<int64_t>(rng() % 5) - 2;
            default: return static_cast<int64_t>(rng());
        }
    }

    // Short strings over the escaped bytes and their neighbours, so shared prefixes and ties are common.
    std::string random_bytes(std::mt19937_64& rng) {
        const char alphabet[] = {'\x00', '\x01', '\x02', 'a', '\xff'};
        std::string s(rng() % 5, '\0');
        for (char& c : s) {
            c = alphabet[rng() % std::size(alphabet)];
        }
        return s;
    }

    std::vector<Value> random_row(const std::vector<ValueType>& schema, std::mt19937_64& rng) {
        std::vector<Value> row;
        for (ValueType type : schema) {
            row.push_back(type == INT_64 ? Value::make_int64(random_int64(rng)) : Value::make_bytes(random_bytes(rng)));
        }
        return row;
    }

    std::vector<Value> typed_slots(const std::vector<ValueType>& schema) {
        std::vector<Value> slots(schema.size());
        for (size_t i = 0; i < schema.size(); ++i) {
            slots[i].type = schema[i];
        }
        return slots;
    }

    const std::vector<std::vector<ValueType>> SCHEMAS = {
        {INT_64},
        {BYTES},
        {INT_64, BYTES},
        {BYTES, INT_64},
        {BYTES, BYTES, INT_64},
    };
}

// ============================================================================
// int64
// ============================================================================
TEST(EncodeInt64Test, WhenInt64IsEncodedThenItIsBigEndianWithTheSignBitFlipped) {
    EXPECT_EQ(enc_int64(0), std::string("\x80\0\0\0\0\0\0\0", 8));
    EXPECT_EQ(enc_int64(1), std::string("\x80\0\0\0\0\0\0\x01", 8));
    EXPECT_EQ(enc_int64(-1), std::string("\x7f\xff\xff\xff\xff\xff\xff\xff", 8));
    EXPECT_EQ(enc_int64(I64_MIN), std::string(8, '\x00'));
    EXPECT_EQ(enc_int64(I64_MAX), std::string(8, '\xff'));
}

TEST(EncodeInt64Test, WhenSortedInt64sAreEncodedThenEncodingsAreSortedAndRoundTrip) {
    const int64_t sorted[] = {I64_MIN, I64_MIN + 1, -(1LL << 32), -256, -1, 0, 1, 255, 1LL << 32, I64_MAX - 1, I64_MAX};
    for (size_t i = 0; i < std::size(sorted); ++i) {
        std::string encoded = enc_int64(sorted[i]);
        if (i > 0) {
            EXPECT_LT(enc_int64(sorted[i - 1]), encoded) << sorted[i - 1] << " vs " << sorted[i];
        }
        size_t pos = 0;
        EXPECT_EQ(decode_int64(encoded, &pos), sorted[i]);
        EXPECT_EQ(pos, 8u);
    }
}

TEST(DecodeInt64Test, WhenFewerThanEightBytesRemainThenDecodeInt64Throws) {
    size_t pos = 1;
    EXPECT_THROW(decode_int64(enc_int64(42), &pos), std::invalid_argument);
}

// ============================================================================
// escape_string / encode_bytes
// ============================================================================
TEST(EscapeStringTest, WhenStringHasZeroOrOneBytesThenOnlyThoseAreEscaped) {
    struct Case {
        std::string raw, escaped;
    };
    const Case cases[] = {
        {"", ""},
        {"abc", "abc"},
        {std::string{'\x00'}, std::string{'\x01', '\x01'}},
        {std::string{'\x01'}, std::string{'\x01', '\x02'}},
        {std::string{'\x02'}, std::string{'\x02'}},
        {std::string{'\x01', '\x01'}, std::string{'\x01', '\x02', '\x01', '\x02'}},
        {std::string{'a', '\x00', '\x01', 'b'}, std::string{'a', '\x01', '\x01', '\x01', '\x02', 'b'}},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(escape_string(c.raw), c.escaped);
        EXPECT_EQ(unescape_string(c.escaped), c.raw);
    }
}

TEST(UnescapeStringTest, WhenEscapeSequenceIsMalformedThenUnescapeStringThrows) {
    const std::string malformed[] = {
        std::string{'\x01'},
        std::string{'a', '\x01'},
        std::string{'\x01', '\x00'},
        std::string{'\x01', '\x03'},
    };
    for (const std::string& s : malformed) {
        EXPECT_THROW(unescape_string(s), std::invalid_argument);
    }
}

TEST(EncodeBytesTest, WhenBytesAreEncodedThenTheOnlyZeroByteIsTheTerminator) {
    const std::string raw{'\x00', 'a', '\x01', '\x00'};
    std::string encoded = enc_bytes(raw);
    EXPECT_EQ(encoded.find('\0'), encoded.size() - 1);

    size_t pos = 0;
    EXPECT_EQ(decode_bytes(encoded, &pos), raw);
    EXPECT_EQ(pos, encoded.size());
}

TEST(EncodeBytesTest, WhenSortedStringsAreEncodedThenEncodingsAreSorted) {
    const std::string sorted[] = {
        "",
        std::string{'\x00'},
        std::string{'\x00', '\x00'},
        std::string{'\x00', '\x01'},
        std::string{'\x00', '\x02'},
        std::string{'\x01'},
        std::string{'\x01', '\x00'},
        std::string{'\x02'},
        "a",
        std::string{'a', '\x00'},
        std::string{'a', '\x00', 'b'},
        std::string{'a', '\x01'},
        "ab",
        "\xff",
    };
    for (size_t i = 1; i < std::size(sorted); ++i) {
        ASSERT_LT(compare_bytes(sorted[i - 1], sorted[i]), 0) << "table must be sorted at " << i;
        EXPECT_LT(enc_bytes(sorted[i - 1]), enc_bytes(sorted[i])) << "at " << i;
    }
}

TEST(DecodeBytesTest, WhenTerminatorIsMissingThenDecodeBytesThrows) {
    size_t pos = 0;
    EXPECT_THROW(decode_bytes("abc", &pos), std::invalid_argument);
}

// ============================================================================
// encode_values / decode_values
// ============================================================================
TEST(EncodeValuesTest, WhenCompositeRowsShareAStringPrefixThenEncodingsSortLikeTheRows) {
    const std::vector<std::vector<Value>> sorted = {
        {Value::make_bytes(""), Value::make_int64(I64_MAX)},
        {Value::make_bytes("a"), Value::make_int64(I64_MIN)},
        {Value::make_bytes("a"), Value::make_int64(-1)},
        {Value::make_bytes("a"), Value::make_int64(0)},
        {Value::make_bytes(std::string{'a', '\x00'}), Value::make_int64(I64_MIN)},
        {Value::make_bytes(std::string{'a', '\x01'}), Value::make_int64(0)},
        {Value::make_bytes("b"), Value::make_int64(-5)},
    };
    for (size_t i = 1; i < sorted.size(); ++i) {
        ASSERT_LT(logical_compare(sorted[i - 1], sorted[i]), 0) << "table must be sorted at " << i;
        EXPECT_LT(encode_values(sorted[i - 1]), encode_values(sorted[i])) << describe(sorted[i]);
    }
}

TEST(EncodeValuesTest, WhenRandomRowsAreComparedThenEncodedOrderMatchesLogicalOrder) {
    std::mt19937_64 rng(20260914);
    int counts[3] = {0, 0, 0}; // less, equal, greater: proves the generator covers every outcome
    for (const auto& schema : SCHEMAS) {
        for (int i = 0; i < 2000; ++i) {
            std::vector<Value> a = random_row(schema, rng);
            std::vector<Value> b = random_row(schema, rng);
            int expected = sign(logical_compare(a, b));
            ASSERT_EQ(sign(encode_values(a).compare(encode_values(b))), expected)
                << describe(a) << " vs " << describe(b);
            ++counts[expected + 1];
        }
    }
    EXPECT_GT(counts[0], 0);
    EXPECT_GT(counts[1], 0);
    EXPECT_GT(counts[2], 0);
}

TEST(DecodeValuesTest, WhenRandomRowsAreEncodedThenDecodeValuesRestoresThem) {
    std::mt19937_64 rng(7);
    for (const auto& schema : SCHEMAS) {
        for (int i = 0; i < 500; ++i) {
            std::vector<Value> row = random_row(schema, rng);
            std::vector<Value> out = typed_slots(schema);
            decode_values(encode_values(row), &out);
            ASSERT_TRUE(out == row) << describe(row);
        }
    }
}

TEST(DecodeValuesTest, WhenBytesRemainAfterTheLastValueThenDecodeValuesThrows) {
    std::vector<Value> out = typed_slots({INT_64});
    EXPECT_THROW(decode_values(enc_int64(1) + "x", &out), std::invalid_argument);
}

TEST(EncodeValuesTest, WhenAValueHasTheErrorTypeThenEncodeValuesThrows) {
    EXPECT_THROW(encode_values({Value{}}), std::invalid_argument);
}
