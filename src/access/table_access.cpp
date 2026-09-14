#include "table_access.h"

#include "row_codec.h"
#include "scanner.h"
#include "../encoding/order_preserving.h"

namespace {
    std::vector<uint8_t> to_bytes(const std::string& s) {
        return std::vector<uint8_t>(s.begin(), s.end());
    }
}

bool db_get(KV* kv, const TableDef& tdef, Record* rec, std::string* err) {
    Scanner sc(CMP_GE, CMP_LE, *rec, *rec);
    if (!db_scan(kv, tdef, &sc, err) || !sc.valid()) return false;
    sc.deref(rec);
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
