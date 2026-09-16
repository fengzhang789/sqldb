#include "ql/ql_ast.h"

#include <string>
#include <utility>
#include <vector>

namespace {
    // The s-expression head for an interior node; leaves are rendered by to_string itself.
    const char* op_name(QLNodeType type) {
        switch (type) {
            case QL_TUP: return "tuple";
            case QL_CMP_GE: return ">=";
            case QL_CMP_GT: return ">";
            case QL_CMP_LT: return "<";
            case QL_CMP_LE: return "<=";
            case QL_CMP_EQ: return "=";
            case QL_CMP_NE: return "!=";
            case QL_ADD: return "+";
            case QL_SUB: return "-";
            case QL_MUL: return "*";
            case QL_DIV: return "/";
            case QL_MOD: return "%";
            case QL_AND: return "and";
            case QL_OR: return "or";
            case QL_NOT: return "not";
            case QL_NEG: return "neg";
            default: return "?";
        }
    }

    // Renders a string literal the way the lexer accepts it back.
    std::string quote(const std::string& s) {
        std::string out = "'";
        for (char c : s) {
            switch (c) {
                case '\'': out += "\\'"; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\t': out += "\\t"; break;
                case '\r': out += "\\r"; break;
                default: out += c; break;
            }
        }
        return out + "'";
    }
}

bool QLNode::operator==(const QLNode& other) const {
    return type == other.type && val == other.val && kids == other.kids;
}

QLNode ql_int64(int64_t v) {
    QLNode node;
    node.type = QL_I64;
    node.val = Value::make_int64(v);
    return node;
}

QLNode ql_str(std::string v) {
    QLNode node;
    node.type = QL_STR;
    node.val = Value::make_bytes(std::move(v));
    return node;
}

QLNode ql_sym(std::string name) {
    QLNode node;
    node.type = QL_SYM;
    node.val = Value::make_bytes(std::move(name));
    return node;
}

QLNode ql_star() {
    QLNode node;
    node.type = QL_STAR;
    return node;
}

QLNode ql_unop(QLNodeType type, QLNode kid) {
    QLNode node;
    node.type = type;
    node.kids.push_back(std::move(kid));
    return node;
}

QLNode ql_binop(QLNodeType type, QLNode lhs, QLNode rhs) {
    QLNode node;
    node.type = type;
    node.kids.push_back(std::move(lhs));
    node.kids.push_back(std::move(rhs));
    return node;
}

QLNode ql_tuple(std::vector<QLNode> kids) {
    QLNode node;
    node.type = QL_TUP;
    node.kids = std::move(kids);
    return node;
}

std::string to_string(const QLNode& node) {
    switch (node.type) {
        case QL_UNINIT: return "()";
        case QL_I64: return std::to_string(node.val.int64);
        case QL_STR: return quote(node.val.str);
        case QL_SYM: return node.val.str;
        case QL_STAR: return "*";
        default: break;
    }

    std::string out = std::string("(") + op_name(node.type);
    for (const QLNode& kid : node.kids) {
        out += " " + to_string(kid);
    }
    return out + ")";
}
