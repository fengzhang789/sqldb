#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../catalog/value.h"

// Order-preserving encoding: for values of the same types, comparing the encoded bytes orders them the same way as
// comparing the values, so encoded primary keys sort correctly in the B+tree. Decoders throw std::invalid_argument
// on malformed input and advance *pos past what they consume.

// Sign bit flipped so negatives sort first, then 8 big-endian bytes.
void encode_int64(std::string* out, int64_t v);
int64_t decode_int64(const std::string& data, size_t* pos);

// 0x00 -> 0x01 0x01 and 0x01 -> 0x01 0x02, so escaped strings contain no 0x00 byte.
// This is needed so that 0x00 does not terminate a string early
std::string escape_string(const std::string& s);
std::string unescape_string(const std::string& s);

// Escaped string followed by a 0x00 terminator.
void encode_bytes(std::string* out, const std::string& s);
std::string decode_bytes(const std::string& data, size_t* pos);

// Concatenated encodings with no type tags, so decode_values needs `out` pre-sized/typed by the caller and must
// consume all of `data`.
std::string encode_values(const std::vector<Value>& values);
void decode_values(const std::string& data, std::vector<Value>* out);
