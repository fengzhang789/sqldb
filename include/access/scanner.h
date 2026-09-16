#pragma once

#include <string>
#include <utility>
#include <vector>

#include "catalog/record.h"
#include "catalog/tabledef.h"
#include "storage/btree_iter.h"
#include "storage/kv.h"

// Scanner is a range query over a table's primary key or one of its secondary indexes: it starts at the row closest to
// key1 satisfying cmp1, then walks toward key2 (forward if cmp1 > 0, backward otherwise) while rows satisfy cmp2
// relative to key2. A bound naming only a prefix of the index's columns stands for every key sharing that prefix.
struct Scanner {
    CMP cmp1;
    CMP cmp2;
    Record key1; // a prefix of the pk or of an index's columns, in that order; picks the index (see find_index)
    Record key2; // a prefix of the same columns
    int index_no = -1; // set by db_scan: -1 for the primary key, else tdef.indexes[index_no]

    Scanner(CMP cmp1, CMP cmp2, Record key1, Record key2)
        : cmp1(cmp1), cmp2(cmp2), key1(std::move(key1)), key2(std::move(key2)) {}

    bool valid() const; // is the current row within the range?
    void next(); // move toward key2; requires valid()
    void deref(Record* rec) const; // replaces *rec with the current row, in tdef column order; requires valid()

private:
    friend bool db_scan(KVReader* tx, const TableDef& tdef, Scanner* req, std::string* err);
    KVReader* tx_ = nullptr; // index scans fetch each row through this by primary key
    const TableDef* tdef_ = nullptr;
    BIter iter_;
    std::vector<uint8_t> key_end_; // encoded key2
};

// Positions req at the first row of its range; false with *err set if cmp1/cmp2 don't point in opposite directions,
// no index starts with key1's columns, or either bound doesn't fit that index's columns and types. tx and tdef must
// outlive req, and tx must not write while req is in use.
bool db_scan(KVReader* tx, const TableDef& tdef, Scanner* req, std::string* err);
