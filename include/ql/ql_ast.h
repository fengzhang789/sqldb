#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "catalog/tabledef.h"
#include "catalog/value.h"
#include "storage/kv.h" // UpdateMode

// Node tags for QLNode. Literals and column references are leaves; every other tag combines its kids.
enum QLNodeType : int {
    QL_UNINIT = 0, // an absent clause, e.g. a QLScan with no WHERE

    // Leaves.
    QL_I64, // integer literal, in val
    QL_STR, // string literal, in val
    QL_SYM, // column name, in val.str
    QL_STAR, // `*` in a SELECT list; the executor expands it against the TableDef

    // Interior nodes.
    QL_TUP, // (a, b, c); two or more kids

    QL_CMP_GE, // >=
    QL_CMP_GT, // >
    QL_CMP_LT, // <
    QL_CMP_LE, // <=
    QL_CMP_EQ, // =
    QL_CMP_NE, // !=

    QL_ADD,
    QL_SUB,
    QL_MUL,
    QL_DIV,
    QL_MOD,

    QL_AND,
    QL_OR,
    QL_NOT, // one kid
    QL_NEG, // unary minus, one kid
};

// QLNode is one node of an expression tree: a tag plus its kids. A leaf carries its payload in val, as the book does
// by embedding a Value: QL_I64/QL_STR hold the literal, and QL_SYM the column name in val.str (its BYTES type says
// nothing about the column's type - only the executor, which has the TableDef, knows that).
struct QLNode {
    QLNodeType type = QL_UNINIT;
    Value val;
    std::vector<QLNode> kids;

    bool operator==(const QLNode& other) const;
};

// Node constructors, one per shape.
QLNode ql_int64(int64_t v);
QLNode ql_str(std::string v);
QLNode ql_sym(std::string name);
QLNode ql_star();
QLNode ql_unop(QLNodeType type, QLNode kid);
QLNode ql_binop(QLNodeType type, QLNode lhs, QLNode rhs);
QLNode ql_tuple(std::vector<QLNode> kids);

// An s-expression rendering of the tree, e.g. "(+ 1 (* 2 3))". For test assertions and debugging only.
std::string to_string(const QLNode& node);

// QLScan is the row source shared by SELECT/UPDATE/DELETE. key1/key2 are the book's INDEX BY range bounds; this
// parser implements WHERE instead (see ql/ql_parse.h) and always leaves them QL_UNINIT, but they stay in the struct
// so the executor sees one scan shape either way and reads an uninitialized range as "scan everything, then filter".
struct QLScan {
    std::string table;
    QLNode key1; // INDEX BY lower bound; always QL_UNINIT here
    QLNode key2; // INDEX BY upper bound; always QL_UNINIT here
    QLNode filter; // WHERE (or its FILTER spelling); QL_UNINIT when the clause is absent
    int64_t offset = 0;
    int64_t limit = -1; // -1: unlimited
};

// SELECT: names and output are parallel, one entry per select item. An item without AS takes its own column name if
// it is a bare column, a positional "col<N>" otherwise; `*` is the single name "*".
struct QLSelect {
    QLScan scan;
    std::vector<std::string> names;
    std::vector<QLNode> output;
};

// INSERT: values holds one entry per VALUES row, each parallel to names. mode is the repo's own UpdateMode so the
// executor can hand it straight to db_update; the grammar only spells plain INSERT, so it is always INSERT_ONLY.
struct QLInsert {
    std::string table;
    UpdateMode mode = UpdateMode::INSERT_ONLY;
    std::vector<std::string> names;
    std::vector<std::vector<QLNode>> values;
};

// UPDATE: names and values are parallel, one entry per SET assignment.
struct QLUpdate {
    QLScan scan;
    std::vector<std::string> names;
    std::vector<QLNode> values;
};

struct QLDelete {
    QLScan scan;
};

// CREATE TABLE. The parser fills name, cols, types and pkeys, moving the PRIMARY KEY columns to the front as TableDef
// requires. Anything needing the catalog - prefixes, table_def_check - is left to the executor.
struct QLCreateTable {
    TableDef def;
};

using QLStatement = std::variant<QLCreateTable, QLSelect, QLInsert, QLUpdate, QLDelete>;
