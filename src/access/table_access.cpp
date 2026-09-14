#include "table_access.h"

#include "row_codec.h"

namespace {
    std::vector<uint8_t> to_bytes(const std::string& s) {
        return std::vector<uint8_t>(s.begin(), s.end());
    }

    std::string to_string(const std::vector<uint8_t>& b) {
        return std::string(b.begin(), b.end());
    }
}

bool db_get(KV* kv, const TableDef& tdef, Record* rec, std::string* err) {
    std::vector<Value> pk_values;
    if (!check_record(tdef, *rec, tdef.pkeys, &pk_values, err)) return false;

    std::string key = encode_key(tdef.prefix, pk_values);
    auto val_bytes = kv->get(to_bytes(key));
    if (!val_bytes.has_value()) return false;

    size_t ncols = tdef.cols.size() - tdef.pkeys;
    std::vector<Value> col_values(ncols);
    for (size_t i = 0; i < ncols; ++i) {
        col_values[i].type = tdef.types[tdef.pkeys + i];
    }
    decode_values(to_string(*val_bytes), &col_values);

    for (size_t i = 0; i < ncols; ++i) {
        const std::string& col = tdef.cols[tdef.pkeys + i];
        if (col_values[i].type == INT_64) {
            rec->add_int64(col, col_values[i].int64);
        } else {
            rec->add_str(col, col_values[i].str);
        }
    }
    return true;
}

bool db_update(KV* kv, const TableDef& tdef, const Record& rec, UpdateMode mode, std::string* err) {
    std::vector<Value> values;
    if (!check_record(tdef, rec, static_cast<int>(tdef.cols.size()), &values, err)) return false;

    std::vector<Value> pk_values(values.begin(), values.begin() + tdef.pkeys);
    std::vector<Value> col_values(values.begin() + tdef.pkeys, values.end());

    std::string key = encode_key(tdef.prefix, pk_values);
    std::vector<uint8_t> key_bytes = to_bytes(key);
    bool exists = kv->get(key_bytes).has_value();

    if (mode == UpdateMode::INSERT_ONLY && exists) {
        *err = "row already exists in table: " + tdef.name;
        return false;
    }
    if (mode == UpdateMode::UPDATE_ONLY && !exists) {
        *err = "row does not exist in table: " + tdef.name;
        return false;
    }

    kv->set(key_bytes, to_bytes(encode_values(col_values)));
    return true;
}

bool db_delete(KV* kv, const TableDef& tdef, const Record& rec, std::string* err) {
    std::vector<Value> pk_values;
    if (!check_record(tdef, rec, tdef.pkeys, &pk_values, err)) return false;

    std::string key = encode_key(tdef.prefix, pk_values);
    return kv->del(to_bytes(key));
}
