#pragma once

#include <cstdint>
#include <string>

enum ValueType {
    ERROR,
    INT_64,
    BYTES,
};

// Value is a tagged union: only the field matching `type` is meaningful.
struct Value {
    ValueType type = ERROR;
    int64_t int64 = 0;
    std::string str;

    static Value make_int64(int64_t v);
    static Value make_bytes(std::string v);

    bool operator==(const Value& other) const;
};
