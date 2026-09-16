#pragma once

#include <span>
#include <string>
#include <vector>

#include "catalog/record.h"
#include "catalog/tabledef.h"
#include "storage/kv.h"

// Point query by primary key (rec supplies exactly the pk columns, in any order), run as the range scan [rec, rec]; on
// a hit, *rec is replaced by the full row in tdef column order. Returns false with *err unset if the row doesn't
// exist, or false with *err set if rec is malformed.
bool db_get(KVReader* tx, const TableDef& tdef, Record* rec, std::string* err);

// Insert/update a full row (rec must supply every column) per mode, keeping secondary indexes in sync in the same tx;
// false with *err set if rec is malformed or mode's existence requirement isn't met. A throw (e.g. an index key over
// BTREE_MAX_KEY_SIZE) can leave the row written without all of its index keys, so abort tx then.
bool db_update(KVTX* tx, const TableDef& tdef, const Record& rec, UpdateMode mode, std::string* err);

// Point delete by primary key: `rec` must supply exactly the pk columns. Also removes the row's index keys in tx.
bool db_delete(KVTX* tx, const TableDef& tdef, const Record& rec, std::string* err);

// The tree a range key over `keys` scans: -1 for the primary key (also for empty keys, i.e. a full scan), i >= 0 for
// the shortest tdef.indexes[i] starting with keys, or -2 with *err set if nothing starts with keys.
int find_index(const TableDef& tdef, const std::vector<std::string>& keys, std::string* err);

// Does long_cols start with short_cols?
bool is_prefix(std::span<const std::string> long_cols, std::span<const std::string> short_cols);
