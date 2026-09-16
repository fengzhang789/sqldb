#include "access/scanner.h"

#include <cassert>
#include <span>

#include "access/row_codec.h"
#include "access/table_access.h"
#include "catalog/catalog.h"

namespace {
    std::vector<uint8_t> to_bytes(const std::string& s) {
        return std::vector<uint8_t>(s.begin(), s.end());
    }

    // A range bound must name a prefix of index_cols, in order, with matching types.
    bool check_bound(const TableDef& tdef, std::span<const std::string> index_cols, const Record& key,
                     std::string* err) {
        if (!is_prefix(index_cols, key.cols)) {
            *err = "range key columns must be a prefix of the scanned index";
            return false;
        }
        for (size_t i = 0; i < key.cols.size(); ++i) {
            if (key.vals[i].type != tdef.types[col_index(tdef, key.cols[i])]) {
                *err = "type mismatch for column: " + key.cols[i];
                return false;
            }
        }
        return true;
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
    if (index_no < 0) {
        decode_row(*tdef_, std::string(key.begin(), key.end()), std::string(val.begin(), val.end()), rec);
        return;
    }

    // An index key holds the indexed columns (pk included) and no value, so the row is fetched by its primary key.
    assert(val.empty());
    Record index_rec;
    index_rec.cols = tdef_->indexes[index_no];
    for (const std::string& col : index_rec.cols) {
        index_rec.vals.push_back(Value{.type = tdef_->types[col_index(*tdef_, col)]});
    }
    decode_key(std::string(key.begin(), key.end()), &index_rec.vals);

    Record row;
    for (int i = 0; i < tdef_->pkeys; ++i) {
        const Value* v = index_rec.get(tdef_->cols[i]);
        assert(v != nullptr);
        row.cols.push_back(tdef_->cols[i]);
        row.vals.push_back(*v);
    }
    std::string err;
    [[maybe_unused]] bool found = db_get(tx_, *tdef_, &row, &err);
    assert(found && "index key without a matching row");
    *rec = std::move(row);
}

bool db_scan(KVReader* tx, const TableDef& tdef, Scanner* req, std::string* err) {
    req->iter_ = BIter{}; // a failed scan leaves req invalid
    if (!(req->cmp1 > 0 && req->cmp2 < 0) && !(req->cmp1 < 0 && req->cmp2 > 0)) {
        *err = "bad range: cmp1 and cmp2 must point in opposite directions";
        return false;
    }

    int index_no = find_index(tdef, req->key1.cols, err);
    if (index_no < -1) return false;
    bool by_pk = index_no < 0;
    uint32_t prefix = by_pk ? tdef.prefix : tdef.index_prefixes[index_no];
    std::span<const std::string> index_cols =
        by_pk ? std::span(tdef.cols).first(static_cast<size_t>(tdef.pkeys)) : std::span(tdef.indexes[index_no]);
    if (!check_bound(tdef, index_cols, req->key1, err) || !check_bound(tdef, index_cols, req->key2, err)) {
        return false;
    }

    req->tx_ = tx;
    req->tdef_ = &tdef;
    req->index_no = index_no;
    req->key_end_ = to_bytes(encode_key_partial(prefix, req->key2.vals, tdef, index_cols, req->cmp2));
    req->iter_ = tx->seek(to_bytes(encode_key_partial(prefix, req->key1.vals, tdef, index_cols, req->cmp1)), req->cmp1);
    return true;
}
