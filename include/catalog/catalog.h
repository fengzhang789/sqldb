#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "catalog/tabledef.h"
#include "storage/kv.h"

// @meta is a bootstrap table used by the DB itself to store arbitrary metadata, e.g. the next unallocated prefix.
extern const TableDef TDEF_META;

// @table maps table name -> serialized TableDef, so we can keep track of table definitions.
extern const TableDef TDEF_TABLE;

constexpr uint32_t TABLE_PREFIX_MIN = 3; // 1 and 2 are reserved for @meta/@table

// Position of col in tdef.cols, or -1 if absent.
int col_index(const TableDef& tdef, const std::string& col);

// Validates an index's columns (non-empty, known, no duplicates) and writes them to *out with any missing pk columns
// appended, which makes every index key unique. False with *err set if invalid or the result covers every column.
bool check_index_keys(const TableDef& tdef, const std::vector<std::string>& index, std::vector<std::string>* out,
                      std::string* err);

// Basic schema validation (non-empty name/cols, pkeys in range, etc), normalizing tdef->indexes via check_index_keys.
bool table_def_check(TableDef* tdef, std::string* err);

// Catalog owns the table-definition cache and schema lifecycle. It reads/writes @meta/@table through small
// self-contained KV helpers rather than access/'s generic row codec, since access/ depends on catalog/ for
// TableDef and going the other way would be circular.
struct Catalog {
    // Checks the in-memory cache, falling back to a read from @table through tx. Returns nullptr if the table doesn't
    // exist. Only write transactions use the cache, and KV runs them one at a time, so it needs no lock of its own.
    const TableDef* get_table_def(KVTX* tx, const std::string& name);

    // Validates the schema, allocates KV key prefixes for the table and each index, and writes the definition into
    // @table, all through tx. Returns false (and sets *err) on failure (e.g. table already exists).
    bool table_new(KVTX* tx, TableDef def, std::string* err);

    // Drops every cached def (invalidating pointers from get_table_def), e.g. once a rollback may have removed one.
    void clear_cache();

    // Reads a TableDef straight from @table through tx, bypassing the cache, as readers do.
    std::unique_ptr<TableDef> get_table_def_from_kv(KVReader* tx, const std::string& name);

private:
    // Allocates n consecutive prefixes from @meta["next_prefix"] (default TABLE_PREFIX_MIN if absent); returns the first.
    uint32_t alloc_prefix(KVTX* tx, uint32_t n);

    // Raw KV access to @meta/@table: key = 4-byte big-endian prefix + primary-key bytes.
    std::optional<std::string> internal_get(KVReader* tx, const TableDef& tdef, const std::string& pk);
    void internal_set(KVTX* tx, const TableDef& tdef, const std::string& pk, const std::string& val);

    std::unordered_map<std::string, std::unique_ptr<TableDef>> cache_;
};
