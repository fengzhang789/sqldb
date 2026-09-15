#include "catalog/catalog.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "catalog/tabledef.h"

namespace {
    // key = 4-byte big-endian prefix + primary-key bytes.
    std::vector<uint8_t> internal_key(uint32_t prefix, const std::string& pk) {
        std::vector<uint8_t> key(4);
        key[0] = static_cast<uint8_t>(prefix >> 24);
        key[1] = static_cast<uint8_t>(prefix >> 16);
        key[2] = static_cast<uint8_t>(prefix >> 8);
        key[3] = static_cast<uint8_t>(prefix);
        key.insert(key.end(), pk.begin(), pk.end());
        return key;
    }

    bool contains(const std::vector<std::string>& cols, const std::string& col) {
        return std::find(cols.begin(), cols.end(), col) != cols.end();
    }
}

const TableDef TDEF_META = TableDefBuilder("@meta").add_col("key", BYTES).add_col("val", BYTES)
    .set_pkeys(1).set_prefix(1).build();
const TableDef TDEF_TABLE = TableDefBuilder("@table").add_col("name", BYTES).add_col("def", BYTES)
    .set_pkeys(1).set_prefix(2).build();

int col_index(const TableDef& tdef, const std::string& col) {
    for (size_t i = 0; i < tdef.cols.size(); ++i) {
        if (tdef.cols[i] == col) return static_cast<int>(i);
    }
    return -1;
}

bool check_index_keys(const TableDef& tdef, const std::vector<std::string>& index, std::vector<std::string>* out,
                      std::string* err) {
    if (index.empty()) {
        *err = "index must have at least one column";
        return false;
    }

    out->clear();
    for (const std::string& col : index) {
        if (col_index(tdef, col) < 0) {
            *err = "unknown index column: " + col;
            return false;
        }
        if (contains(*out, col)) {
            *err = "duplicate index column: " + col;
            return false;
        }
        out->push_back(col);
    }
    for (int i = 0; i < tdef.pkeys; ++i) {
        if (!contains(*out, tdef.cols[i])) out->push_back(tdef.cols[i]);
    }

    if (out->size() >= tdef.cols.size()) {
        *err = "index plus primary key must not cover every column";
        return false;
    }
    return true;
}

bool table_def_check(TableDef* tdef, std::string* err) {
    if (tdef->name.empty()) {
        *err = "table name must not be empty";
        return false;
    }
    if (tdef->cols.size() != tdef->types.size()) {
        *err = "cols and types must have the same size";
        return false;
    }
    if (tdef->pkeys <= 0 || static_cast<size_t>(tdef->pkeys) > tdef->cols.size()) {
        *err = "pkeys must be in (0, cols.size()]";
        return false;
    }
    for (std::vector<std::string>& index : tdef->indexes) {
        std::vector<std::string> normalized;
        if (!check_index_keys(*tdef, index, &normalized, err)) return false;
        index = std::move(normalized);
    }
    return true;
}

std::optional<std::string> Catalog::internal_get(const TableDef& tdef, const std::string& pk) {
    auto val = kv_->get(internal_key(tdef.prefix, pk));
    if (!val.has_value()) return std::nullopt;
    return std::string(val->begin(), val->end());
}

void Catalog::internal_set(const TableDef& tdef, const std::string& pk, const std::string& val) {
    kv_->set(internal_key(tdef.prefix, pk), std::vector<uint8_t>(val.begin(), val.end()));
}

std::unique_ptr<TableDef> Catalog::get_table_def_from_kv(const std::string& name) {
    auto encoded = internal_get(TDEF_TABLE, name);
    if (!encoded.has_value()) return nullptr;

    auto def = std::make_unique<TableDef>();
    if (!decode_table_def(*encoded, def.get())) return nullptr;
    return def;
}

const TableDef* Catalog::get_table_def(const std::string& name) {
    auto it = cache_.find(name);
    if (it != cache_.end()) return it->second.get();

    auto def = get_table_def_from_kv(name);
    if (!def) return nullptr;

    const TableDef* ptr = def.get();
    cache_[name] = std::move(def);
    return ptr;
}

// next_prefix is stored as a 4-byte big-endian uint32 under @meta["next_prefix"].
uint32_t Catalog::alloc_prefix(uint32_t n) {
    uint32_t next = TABLE_PREFIX_MIN;
    auto stored = internal_get(TDEF_META, "next_prefix");
    if (stored.has_value() && stored->size() == 4) {
        const auto& s = *stored;
        next = (static_cast<uint8_t>(s[0]) << 24) | (static_cast<uint8_t>(s[1]) << 16) |
               (static_cast<uint8_t>(s[2]) << 8) | static_cast<uint8_t>(s[3]);
    }

    uint32_t following = next + n;
    std::string encoded(4, '\0');
    encoded[0] = static_cast<char>(following >> 24);
    encoded[1] = static_cast<char>(following >> 16);
    encoded[2] = static_cast<char>(following >> 8);
    encoded[3] = static_cast<char>(following);
    internal_set(TDEF_META, "next_prefix", encoded);

    return next;
}

bool Catalog::table_new(TableDef def, std::string* err) {
    if (!table_def_check(&def, err)) return false;
    if (def.prefix != 0 || !def.index_prefixes.empty()) {
        *err = "prefixes must not be set by the caller";
        return false;
    }

    if (internal_get(TDEF_TABLE, def.name).has_value()) {
        *err = "table already exists: " + def.name;
        return false;
    }

    // One prefix for the table's rows, then one per index.
    def.prefix = alloc_prefix(1 + static_cast<uint32_t>(def.indexes.size()));
    for (size_t i = 0; i < def.indexes.size(); ++i) {
        def.index_prefixes.push_back(def.prefix + 1 + static_cast<uint32_t>(i));
    }
    internal_set(TDEF_TABLE, def.name, encode_table_def(def));
    return true;
}
