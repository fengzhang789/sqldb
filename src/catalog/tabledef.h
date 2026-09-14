#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "value.h"

struct TableDef {
    std::string name; // table name
    std::vector<ValueType> types; // column types, parallel to cols
    std::vector<std::string> cols; // column names
    int pkeys = 0; // the first pkeys columns in cols form the primary key
    uint32_t prefix = 0; // auto assigned unique prefix for the PK to avoid collision
};

// Serialize/deserialize a TableDef to/from its on-disk representation.
// TODO: later: protobuf?
std::string encode_table_def(const TableDef& def);
bool decode_table_def(const std::string& data, TableDef* out);

// Builder class for a TableDef.
class TableDefBuilder {
    public:
        explicit TableDefBuilder(std::string name);
        TableDefBuilder& add_col(std::string name, ValueType type);
        TableDefBuilder& set_pkeys(int pkeys);
        TableDefBuilder& set_prefix(uint32_t prefix);
        TableDef build() const;

    private:
        TableDef def_;
};
