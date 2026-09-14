#include "scanner.h"

#include <cassert>

#include "row_codec.h"
#include "../encoding/order_preserving.h"

namespace {
    std::vector<uint8_t> to_bytes(const std::string& s) {
        return std::vector<uint8_t>(s.begin(), s.end());
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

bool Scanner::valid() const {
    return iter_.valid() && cmp_ok(iter_.deref().first, cmp2, key_end_);
}

void Scanner::next() {
    assert(valid());
    if (cmp1 > 0) {
        iter_.next();
    } else {
        iter_.prev();
    }
}

void Scanner::deref(Record* rec) const {
    assert(valid());
    auto [key, val] = iter_.deref();
    size_t pkeys = static_cast<size_t>(tdef_->pkeys);
    std::vector<Value> pk_values = typed_slots(*tdef_, 0, pkeys);
    std::vector<Value> col_values = typed_slots(*tdef_, pkeys, tdef_->cols.size());
    decode_key(std::string(key.begin(), key.end()), &pk_values);
    decode_values(std::string(val.begin(), val.end()), &col_values);

    rec->cols = tdef_->cols;
    rec->vals = std::move(pk_values);
    rec->vals.insert(rec->vals.end(), col_values.begin(), col_values.end());
}

bool db_scan(KV* kv, const TableDef& tdef, Scanner* req, std::string* err) {
    req->iter_ = BIter{}; // a failed scan leaves req invalid
    if (!(req->cmp1 > 0 && req->cmp2 < 0) && !(req->cmp1 < 0 && req->cmp2 > 0)) {
        *err = "bad range: cmp1 and cmp2 must point in opposite directions";
        return false;
    }

    std::vector<Value> start, end;
    if (!check_record(tdef, req->key1, tdef.pkeys, &start, err)) return false;
    if (!check_record(tdef, req->key2, tdef.pkeys, &end, err)) return false;

    req->tdef_ = &tdef;
    req->key_end_ = to_bytes(encode_key(tdef.prefix, end));
    req->iter_ = kv->seek(to_bytes(encode_key(tdef.prefix, start)), req->cmp1);
    return true;
}
