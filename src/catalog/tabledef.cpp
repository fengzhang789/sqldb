#include "catalog/tabledef.h"

#include <sstream>

std::string encode_table_def(const TableDef& def) {
    std::ostringstream out;
    out << def.name << '\n'
        << def.pkeys << '\n'
        << def.prefix << '\n'
        << def.cols.size() << '\n';
    for (size_t i = 0; i < def.cols.size(); ++i) {
        out << def.cols[i] << ' ' << static_cast<uint32_t>(def.types[i]) << '\n';
    }
    out << def.indexes.size() << '\n';
    for (const std::vector<std::string>& index : def.indexes) {
        out << index.size();
        for (const std::string& col : index) {
            out << ' ' << col;
        }
        out << '\n';
    }
    out << def.index_prefixes.size();
    for (uint32_t prefix : def.index_prefixes) {
        out << ' ' << prefix;
    }
    out << '\n';
    return out.str();
}

bool decode_table_def(const std::string& data, TableDef* out) {
    std::istringstream in(data);
    size_t ncols = 0;
    if (!std::getline(in, out->name)) return false;
    if (!(in >> out->pkeys)) return false;
    if (!(in >> out->prefix)) return false;
    if (!(in >> ncols)) return false;
    in.ignore();  // consume trailing newline

    out->cols.clear();
    out->types.clear();
    for (size_t i = 0; i < ncols; ++i) {
        std::string col;
        uint32_t type_raw;
        if (!(in >> col >> type_raw)) return false;
        out->cols.push_back(col);
        out->types.push_back(static_cast<ValueType>(type_raw));
    }

    size_t nindexes = 0;
    if (!(in >> nindexes)) return false;
    out->indexes.clear();
    for (size_t i = 0; i < nindexes; ++i) {
        size_t n = 0;
        if (!(in >> n)) return false;
        std::vector<std::string> index;
        for (size_t j = 0; j < n; ++j) {
            std::string col;
            if (!(in >> col)) return false;
            index.push_back(col);
        }
        out->indexes.push_back(std::move(index));
    }

    size_t nprefixes = 0;
    if (!(in >> nprefixes)) return false;
    out->index_prefixes.clear();
    for (size_t i = 0; i < nprefixes; ++i) {
        uint32_t prefix;
        if (!(in >> prefix)) return false;
        out->index_prefixes.push_back(prefix);
    }
    return true;
}

TableDefBuilder::TableDefBuilder(std::string name) {
    def_.name = std::move(name);
    def_.pkeys = 0;
    def_.prefix = 0;
}

TableDefBuilder& TableDefBuilder::add_col(std::string name, ValueType type) {
    def_.cols.push_back(std::move(name));
    def_.types.push_back(type);
    return *this;
}

TableDefBuilder& TableDefBuilder::set_pkeys(int pkeys) {
    def_.pkeys = pkeys;
    return *this;
}

TableDefBuilder& TableDefBuilder::add_index(std::vector<std::string> cols) {
    def_.indexes.push_back(std::move(cols));
    return *this;
}

TableDefBuilder& TableDefBuilder::set_prefix(uint32_t prefix) {
    def_.prefix = prefix;
    return *this;
}

TableDef TableDefBuilder::build() const {
    return def_;
}
