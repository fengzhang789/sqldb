#include "row_codec.h"

#include <stdexcept>

namespace {
    void append_u32_be(std::string* out, uint32_t v) {
        out->push_back(static_cast<char>(v >> 24));
        out->push_back(static_cast<char>(v >> 16));
        out->push_back(static_cast<char>(v >> 8));
        out->push_back(static_cast<char>(v));
    }

    uint32_t read_u32_be(const std::string& data, size_t pos) {
        return (static_cast<uint8_t>(data[pos]) << 24) | (static_cast<uint8_t>(data[pos + 1]) << 16) |
               (static_cast<uint8_t>(data[pos + 2]) << 8) | static_cast<uint8_t>(data[pos + 3]);
    }

    // No type tag on disk: the caller always knows the type at each position.
    void encode_value(std::string* out, const Value& v) {
        switch (v.type) {
            case INT_64: {
                uint64_t bits = static_cast<uint64_t>(v.int64);
                for (int shift = 56; shift >= 0; shift -= 8) {
                    out->push_back(static_cast<char>(bits >> shift));
                }
                break;
            }
            case BYTES:
                append_u32_be(out, static_cast<uint32_t>(v.str.size()));
                out->append(v.str);
                break;
            case ERROR:
                throw std::invalid_argument("row_codec: cannot encode an ERROR value");
        }
    }

    // Decodes the value at `*pos` in `data` according to `out->type`, advancing `*pos`.
    void decode_value(const std::string& data, size_t* pos, Value* out) {
        switch (out->type) {
            case INT_64: {
                uint64_t bits = 0;
                for (int i = 0; i < 8; ++i) {
                    bits = (bits << 8) | static_cast<uint8_t>(data[*pos + i]);
                }
                out->int64 = static_cast<int64_t>(bits);
                *pos += 8;
                break;
            }
            case BYTES: {
                uint32_t len = read_u32_be(data, *pos);
                *pos += 4;
                out->str = data.substr(*pos, len);
                *pos += len;
                break;
            }
            case ERROR:
                throw std::invalid_argument("row_codec: cannot decode into a ERROR slot");
        }
    }
}

std::string encode_key(uint32_t prefix, const std::vector<Value>& pk_values) {
    std::string out;
    append_u32_be(&out, prefix);
    for (const Value& v : pk_values) {
        encode_value(&out, v);
    }
    return out;
}

std::string encode_values(const std::vector<Value>& values) {
    std::string out;
    for (const Value& v : values) {
        encode_value(&out, v);
    }
    return out;
}

void decode_values(const std::string& data, std::vector<Value>* out) {
    size_t pos = 0;
    for (Value& v : *out) {
        decode_value(data, &pos, &v);
    }
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
