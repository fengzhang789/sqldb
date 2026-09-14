#include "catalog.h"

#include <cstdint>
#include <string>
#include <vector>

#include "tabledef.h"

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
}

const TableDef TDEF_META = TableDefBuilder("@meta").add_col("key", BYTES).add_col("val", BYTES)
    .set_pkeys(1).set_prefix(1).build();
const TableDef TDEF_TABLE = TableDefBuilder("@table").add_col("name", BYTES).add_col("def", BYTES)
    .set_pkeys(1).set_prefix(2).build();

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

bool Catalog::check_table_def(const TableDef& def, std::string* err) {
    if (def.name.empty()) {
        *err = "table name must not be empty";
        return false;
    }
    if (def.cols.size() != def.types.size()) {
        *err = "cols and types must have the same size";
        return false;
    }
    if (def.pkeys <= 0 || static_cast<size_t>(def.pkeys) > def.cols.size()) {
        *err = "pkeys must be in (0, cols.size()]";
        return false;
    }
    if (def.prefix != 0) {
        *err = "prefix must not be set by the caller";
        return false;
    }
    return true;
}

// next_prefix is stored as a 4-byte big-endian uint32 under @meta["next_prefix"].
uint32_t Catalog::alloc_prefix() {
    uint32_t next = TABLE_PREFIX_MIN;
    auto stored = internal_get(TDEF_META, "next_prefix");
    if (stored.has_value() && stored->size() == 4) {
        const auto& s = *stored;
        next = (static_cast<uint8_t>(s[0]) << 24) | (static_cast<uint8_t>(s[1]) << 16) |
               (static_cast<uint8_t>(s[2]) << 8) | static_cast<uint8_t>(s[3]);
    }

    uint32_t following = next + 1;
    std::string encoded(4, '\0');
    encoded[0] = static_cast<char>(following >> 24);
    encoded[1] = static_cast<char>(following >> 16);
    encoded[2] = static_cast<char>(following >> 8);
    encoded[3] = static_cast<char>(following);
    internal_set(TDEF_META, "next_prefix", encoded);

    return next;
}

bool Catalog::table_new(TableDef def, std::string* err) {
    if (!check_table_def(def, err)) return false;

    if (internal_get(TDEF_TABLE, def.name).has_value()) {
        *err = "table already exists: " + def.name;
        return false;
    }

    def.prefix = alloc_prefix();
    internal_set(TDEF_TABLE, def.name, encode_table_def(def));
    return true;
}
