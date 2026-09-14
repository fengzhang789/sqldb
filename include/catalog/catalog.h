#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "catalog/tabledef.h"
#include "storage/kv.h"

// @meta is a bootstrap table used by the DB itself to store arbitrary metadata, e.g. the next unallocated prefix.
extern const TableDef TDEF_META;

// @table maps table name -> serialized TableDef, so we can keep track of table definitions.
extern const TableDef TDEF_TABLE;

constexpr uint32_t TABLE_PREFIX_MIN = 3; // 1 and 2 are reserved for @meta/@table

// Catalog owns the table-definition cache and schema lifecycle. It reads/writes @meta/@table through small
// self-contained KV helpers rather than access/'s generic row codec, since access/ depends on catalog/ for
// TableDef and going the other way would be circular.
struct Catalog {
    explicit Catalog(KV* kv) : kv_(kv) {}

    // Checks the in-memory cache, falling back to a read from @table. Returns nullptr if the table doesn't exist.
    const TableDef* get_table_def(const std::string& name);

    // Validates the schema, allocates the next KV key prefix, and persists the definition into @table.
    // Returns false (and sets *err) on failure (e.g. table already exists).
    bool table_new(TableDef def, std::string* err);

private:
    // Reads a TableDef straight from @table, bypassing the cache.
    std::unique_ptr<TableDef> get_table_def_from_kv(const std::string& name);

    // Basic schema validation (non-empty name/cols, pkeys in range, etc).
    bool check_table_def(const TableDef& def, std::string* err);

    // Allocates and persists the next table prefix from @meta["next_prefix"] (default TABLE_PREFIX_MIN if absent).
    uint32_t alloc_prefix();

    // Raw KV access to @meta/@table: key = 4-byte big-endian prefix + primary-key bytes.
    std::optional<std::string> internal_get(const TableDef& tdef, const std::string& pk);
    void internal_set(const TableDef& tdef, const std::string& pk, const std::string& val);

    KV* kv_;  // not owned
    std::unordered_map<std::string, std::unique_ptr<TableDef>> cache_;
};
