#include "row_codec.h"

#include <stdexcept>

#include "../encoding/order_preserving.h"

namespace {
    constexpr size_t KEY_PREFIX_SIZE = 4; // encode_key's big-endian table prefix

    void append_u32_be(std::string* out, uint32_t v) {
        out->push_back(static_cast<char>(v >> 24));
        out->push_back(static_cast<char>(v >> 16));
        out->push_back(static_cast<char>(v >> 8));
        out->push_back(static_cast<char>(v));
    }
}

std::string encode_key(uint32_t prefix, const std::vector<Value>& pk_values) {
    std::string out;
    append_u32_be(&out, prefix);
    out.append(encode_values(pk_values));
    return out;
}

void decode_key(const std::string& key, std::vector<Value>* out) {
    if (key.size() < KEY_PREFIX_SIZE) {
        throw std::invalid_argument("row_codec: key is shorter than its table prefix");
    }
    decode_values(key.substr(KEY_PREFIX_SIZE), out);
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
