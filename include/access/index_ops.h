#pragma once

#include "catalog/record.h"
#include "catalog/tabledef.h"
#include "storage/kv.h"

constexpr int INDEX_ADD = 1;
constexpr int INDEX_DEL = 2;

// Adds (INDEX_ADD) or removes (INDEX_DEL) rec's key in every secondary index of tdef, asserting each key was absent
// or present beforehand. rec must hold every indexed column, in any order.
void index_op(KV* kv, const TableDef& tdef, const Record& rec, int op);
