#include "catalog/record.h"

Record& Record::add_str(const std::string& col, std::string val) {
    cols.push_back(col);
    vals.push_back(Value::make_bytes(std::move(val)));
    return *this;
}

Record& Record::add_int64(const std::string& col, int64_t val) {
    cols.push_back(col);
    vals.push_back(Value::make_int64(val));
    return *this;
}

const Value* Record::get(const std::string& col) const {
    for (size_t i = 0; i < cols.size(); ++i) {
        if (cols[i] == col) return &vals[i];
    }
    return nullptr;
}
