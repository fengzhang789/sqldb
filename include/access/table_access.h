#pragma once

#include <string>

#include "catalog/record.h"
#include "catalog/tabledef.h"
#include "storage/kv.h"

enum class UpdateMode {
    UPSERT,       // insert or overwrite
    UPDATE_ONLY,  // fail if the row does not exist
    INSERT_ONLY,  // fail if the row already exists
};

// Point query by primary key (rec supplies exactly the pk columns), run as the range scan [rec, rec]; on a hit, *rec
// is replaced by the full row in tdef column order. Returns false with *err unset if the row doesn't exist, or false
// with *err set if rec is malformed.
bool db_get(KV* kv, const TableDef& tdef, Record* rec, std::string* err);

// Insert/update a full row (rec must supply every column) per mode; false with *err set if rec is malformed or
// mode's existence requirement isn't met.
bool db_update(KV* kv, const TableDef& tdef, const Record& rec, UpdateMode mode, std::string* err);

// Point delete by primary key: `rec` must supply exactly the pk columns.
bool db_delete(KV* kv, const TableDef& tdef, const Record& rec, std::string* err);
