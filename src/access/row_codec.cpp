#include "access/row_codec.h"

#include <cassert>
#include <stdexcept>

#include "catalog/catalog.h"
#include "encoding/order_preserving.h"

namespace {
    constexpr size_t KEY_PREFIX_SIZE = 4; // encode_key's big-endian table prefix

    void append_u32_be(std::string* out, uint32_t v) {
        out->push_back(static_cast<char>(v >> 24));
        out->push_back(static_cast<char>(v >> 16));
        out->push_back(static_cast<char>(v >> 8));
        out->push_back(static_cast<char>(v));
    }

    // Value slots typed like tdef's columns [begin, end), ready for decode_values.
    std::vector<Value> typed_slots(const TableDef& tdef, size_t begin, size_t end) {
        std::vector<Value> slots(end - begin);
        for (size_t i = begin; i < end; ++i) {
            slots[i - begin].type = tdef.types[i];
        }
        return slots;
    }
}

std::string encode_key(uint32_t prefix, const std::vector<Value>& pk_values) {
    std::string out;
    append_u32_be(&out, prefix);
    out.append(encode_values(pk_values));
    return out;
}

std::string encode_key_partial(uint32_t prefix, const std::vector<Value>& values, const TableDef& tdef,
                               std::span<const std::string> index_cols, CMP cmp) {
    std::string out = encode_key(prefix, values);
    if (cmp != CMP_GT && cmp != CMP_LE) return out;

    for (size_t i = values.size(); i < index_cols.size(); ++i) {
        int col = col_index(tdef, index_cols[i]);
        assert(col >= 0);
        switch (tdef.types[col]) {
            case INT_64:
                out.append(8, '\xff'); // encode_int64(INT64_MAX)
                break;
            case BYTES:
                out.push_back('\xff'); // no string encoding starts with 0xff, so later columns can't matter
                return out;
            case ERROR:
                throw std::invalid_argument("row_codec: cannot pad an ERROR column");
        }
    }
    return out;
}

void decode_key(const std::string& key, std::vector<Value>* out) {
    if (key.size() < KEY_PREFIX_SIZE) {
        throw std::invalid_argument("row_codec: key is shorter than its table prefix");
    }
    decode_values(key.substr(KEY_PREFIX_SIZE), out);
}

void decode_row(const TableDef& tdef, const std::string& key, const std::string& val, Record* rec) {
    size_t pkeys = static_cast<size_t>(tdef.pkeys);
    std::vector<Value> pk_values = typed_slots(tdef, 0, pkeys);
    std::vector<Value> col_values = typed_slots(tdef, pkeys, tdef.cols.size());
    decode_key(key, &pk_values);
    decode_values(val, &col_values);

    rec->cols = tdef.cols;
    rec->vals = std::move(pk_values);
    rec->vals.insert(rec->vals.end(), col_values.begin(), col_values.end());
}

bool check_record(const TableDef& tdef, const Record& rec, int n, std::vector<Value>* out, std::string* err) {
    out->assign(n, Value{});

    for (int i = 0; i < n; ++i) {
        const std::string& col = tdef.cols[i];
        const Value* v = rec.get(col);
        if (v == nullptr) {
            *err = "missing column: " + col;
            return false;
        }
        if (v->type != tdef.types[i]) {
            *err = "type mismatch for column: " + col;
            return false;
        }
        (*out)[i] = *v;
    }

    for (const std::string& col : rec.cols) {
        bool expected = false;
        for (int i = 0; i < n; ++i) {
            if (tdef.cols[i] == col) {
                expected = true;
                break;
            }
        }
        if (!expected) {
            *err = "unexpected column: " + col;
            return false;
        }
    }

    return true;
}
