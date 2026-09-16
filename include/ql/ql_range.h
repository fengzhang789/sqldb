#pragma once

#include "catalog/record.h"
#include "catalog/tabledef.h"
#include "ql/ql_ast.h"
#include "storage/btree_iter.h" // CMP

// The bounds a Scanner is built from.
struct QLRange {
    CMP cmp1 = CMP_GE;
    CMP cmp2 = CMP_LE;
    Record key1;
    Record key2;
};

// The narrowest range over tdef's primary key or one of its indexes that still contains every row the filter can
// match. key1's columns pick the index (see find_index), so empty keys mean a full primary-key scan.
//
// This only narrows what is read: the caller must still apply the filter to every row, since a range can be wider
// than the condition (`a > 1 AND name = 'x'` scans a > 1 and leaves the name to the filter). It is never narrower.
QLRange ql_range(const QLNode& filter, const TableDef& tdef);
