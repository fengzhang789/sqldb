#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../catalog/record.h"
#include "../catalog/tabledef.h"
#include "../catalog/value.h"

// encode_key/encode_values/decode_values operate on arbitrary user TableDefs.

// 4-byte big-endian prefix followed by the encoded primary-key values.
std::string encode_key(uint32_t prefix, const std::vector<Value>& pk_values);

// Serializes non-key column values into bytes.
std::string encode_values(const std::vector<Value>& values);

// Inverse of encode_values; `out` must be pre-sized/typed by the caller, since the encoding carries no type tags.
void decode_values(const std::string& data, std::vector<Value>* out);

// Reorders rec's columns to match tdef.cols; n == tdef.pkeys requires exactly the pk columns, n == tdef.cols.size()
// requires every column. On success *out holds the first n values in tdef.cols order; false with *err set on a
// missing/extra/mismatched column.
bool check_record(const TableDef& tdef, const Record& rec, int n, std::vector<Value>* out, std::string* err);
