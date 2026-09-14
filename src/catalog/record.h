#pragma once

#include <string>
#include <vector>

#include "value.h"

// Record is a name/value view of a row, in whatever column order the caller built it in.
struct Record {
    std::vector<std::string> cols;
    std::vector<Value> vals;

    Record& add_str(const std::string& col, std::string val);
    Record& add_int64(const std::string& col, int64_t val);

    const Value* get(const std::string& col) const;  // nullptr if not present
};
