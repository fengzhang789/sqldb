#include "value.h"

Value Value::make_int64(int64_t v) {
    Value val;
    val.type = INT_64;
    val.int64 = v;
    return val;
}

Value Value::make_bytes(std::string v) {
    Value val;
    val.type = BYTES;
    val.str = std::move(v);
    return val;
}

bool Value::operator==(const Value& other) const {
    if (type != other.type) return false;
    switch (type) {
        case INT_64: return int64 == other.int64;
        case BYTES: return str == other.str;
        default: return true;
    }
}
