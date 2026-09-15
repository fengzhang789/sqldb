#include "access/table_access.h"

#include <algorithm>

#include "access/index_ops.h"
#include "access/row_codec.h"
#include "access/scanner.h"
#include "encoding/order_preserving.h"

namespace {
    std::vector<uint8_t> to_bytes(const std::string& s) {
        return std::vector<uint8_t>(s.begin(), s.end());
    }
}

bool db_get(KV* kv, const TableDef& tdef, Record* rec, std::string* err) {
    Record key;
    if (!check_record(tdef, *rec, tdef.pkeys, &key.vals, err)) return false;
    key.cols.assign(tdef.cols.begin(), tdef.cols.begin() + tdef.pkeys); // db_scan wants the pk columns in order

    Scanner sc(CMP_GE, CMP_LE, key, key);
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
    InsertReq req;
    req.key = to_bytes(key);
    req.val = to_bytes(encode_values(col_values));
    req.mode = mode;
    if (!kv->update(&req)) {
        *err = (req.added ? "row does not exist in table: " : "row already exists in table: ") + tdef.name;
        return false;
    }
    if (tdef.indexes.empty()) return true;

    if (!req.added) {
        Record old;
        decode_row(tdef, key, std::string(req.old.begin(), req.old.end()), &old);
        index_op(kv, tdef, old, INDEX_DEL);
    }
    index_op(kv, tdef, rec, INDEX_ADD);
    return true;
}

bool db_delete(KV* kv, const TableDef& tdef, const Record& rec, std::string* err) {
    std::vector<Value> pk_values;
    if (!check_record(tdef, rec, tdef.pkeys, &pk_values, err)) return false;

    std::string key = encode_key(tdef.prefix, pk_values);
    DeleteReq req;
    req.key = to_bytes(key);
    if (!kv->del(&req)) return false;

    if (!tdef.indexes.empty()) {
        Record old;
        decode_row(tdef, key, std::string(req.old.begin(), req.old.end()), &old);
        index_op(kv, tdef, old, INDEX_DEL);
    }
    return true;
}

int find_index(const TableDef& tdef, const std::vector<std::string>& keys, std::string* err) {
    if (is_prefix(std::span(tdef.cols).first(static_cast<size_t>(tdef.pkeys)), keys)) {
        return -1;
    }

    int winner = -2;
    for (size_t i = 0; i < tdef.indexes.size(); ++i) {
        if (!is_prefix(tdef.indexes[i], keys)) continue;
        if (winner == -2 || tdef.indexes[i].size() < tdef.indexes[winner].size()) {
            winner = static_cast<int>(i);
        }
    }
    if (winner == -2) {
        *err = "no index starts with the range key's columns in table: " + tdef.name;
    }
    return winner;
}

bool is_prefix(std::span<const std::string> long_cols, std::span<const std::string> short_cols) {
    return short_cols.size() <= long_cols.size() && std::equal(short_cols.begin(), short_cols.end(), long_cols.begin());
}
