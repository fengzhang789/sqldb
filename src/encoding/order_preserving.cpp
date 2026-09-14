#include "encoding/order_preserving.h"

#include <stdexcept>

namespace {
    constexpr uint64_t SIGN_BIT = 1ULL << 63;
}

void encode_int64(std::string* out, int64_t v) {
    uint64_t u = static_cast<uint64_t>(v) + SIGN_BIT;
    for (int shift = 56; shift >= 0; shift -= 8) {
        out->push_back(static_cast<char>(u >> shift));
    }
}

int64_t decode_int64(const std::string& data, size_t* pos) {
    if (*pos + 8 > data.size()) {
        throw std::invalid_argument("order_preserving: truncated int64");
    }
    uint64_t u = 0;
    for (size_t i = 0; i < 8; ++i) {
        u = (u << 8) | static_cast<uint8_t>(data[*pos + i]);
    }
    *pos += 8;
    return static_cast<int64_t>(u - SIGN_BIT);
}

std::string escape_string(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (static_cast<uint8_t>(c) <= 0x01) {
            out.push_back('\x01');
            out.push_back(static_cast<char>(c + 1));
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string unescape_string(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\x01') {
            out.push_back(s[i]);
            continue;
        }
        if (i + 1 == s.size() || (s[i + 1] != '\x01' && s[i + 1] != '\x02')) {
            throw std::invalid_argument("order_preserving: bad escape sequence");
        }
        out.push_back(static_cast<char>(s[++i] - 1));
    }
    return out;
}

void encode_bytes(std::string* out, const std::string& s) {
    out->append(escape_string(s));
    out->push_back('\0');
}

std::string decode_bytes(const std::string& data, size_t* pos) {
    size_t end = data.find('\0', *pos);
    if (end == std::string::npos) {
        throw std::invalid_argument("order_preserving: unterminated bytes");
    }
    std::string s = unescape_string(data.substr(*pos, end - *pos));
    *pos = end + 1;
    return s;
}

std::string encode_values(const std::vector<Value>& values) {
    std::string out;
    for (const Value& v : values) {
        switch (v.type) {
            case INT_64:
                encode_int64(&out, v.int64);
                break;
            case BYTES:
                encode_bytes(&out, v.str);
                break;
            case ERROR:
                throw std::invalid_argument("order_preserving: cannot encode an ERROR value");
        }
    }
    return out;
}

void decode_values(const std::string& data, std::vector<Value>* out) {
    size_t pos = 0;
    for (Value& v : *out) {
        switch (v.type) {
            case INT_64:
                v.int64 = decode_int64(data, &pos);
                break;
            case BYTES:
                v.str = decode_bytes(data, &pos);
                break;
            case ERROR:
                throw std::invalid_argument("order_preserving: cannot decode into an ERROR slot");
        }
    }
    if (pos != data.size()) {
        throw std::invalid_argument("order_preserving: trailing bytes after the last value");
    }
}
