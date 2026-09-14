#pragma once

#include <string>
#include <utility>
#include <vector>

#include "catalog/record.h"
#include "catalog/tabledef.h"
#include "storage/btree_iter.h"
#include "storage/kv.h"

// Scanner is a range query over a table's primary key: it starts at the row closest to key1 satisfying cmp1, then
// walks toward key2 (forward if cmp1 > 0, backward otherwise) while rows satisfy cmp2 relative to key2.
struct Scanner {
    CMP cmp1;
    CMP cmp2;
    Record key1; // exactly the primary-key columns
    Record key2; // exactly the primary-key columns

    Scanner(CMP cmp1, CMP cmp2, Record key1, Record key2)
        : cmp1(cmp1), cmp2(cmp2), key1(std::move(key1)), key2(std::move(key2)) {}

    bool valid() const; // is the current row within the range?
    void next(); // move toward key2; requires valid()
    void deref(Record* rec) const; // replaces *rec with the current row, in tdef column order; requires valid()

private:
    friend bool db_scan(KV* kv, const TableDef& tdef, Scanner* req, std::string* err);
    const TableDef* tdef_ = nullptr;
    BIter iter_;
    std::vector<uint8_t> key_end_; // encoded key2
};

// Positions req at the first row of its range; false with *err set if cmp1/cmp2 don't point in opposite directions
// or key1/key2 aren't exactly the pk columns. kv and tdef must outlive req, and kv must not be updated while in use.
bool db_scan(KV* kv, const TableDef& tdef, Scanner* req, std::string* err);
