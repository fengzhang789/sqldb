#pragma once

#include <string>
#include <utility>

#include "catalog/value.h"
#include "ql/ql_ast.h"

// Glue internal to the parser front-end. Callers use parse_statement in ql/ql_parse.h and never see this.
namespace ql {
    // Threaded through both the generated lexer and the generated parser: where the statement lands, the first error,
    // and the scanner's position, which flex only half tracks (it counts lines, never columns).
    struct ParseContext {
        QLStatement* out = nullptr;
        std::string err;
        int line = 1;
        int col = 1;
    };

    // Grammar-internal pairings, not part of the AST: one SELECT item or SET assignment (an empty name means a SELECT
    // item written without AS), and one CREATE TABLE column.
    using NamedExpr = std::pair<std::string, QLNode>;
    using ColDef = std::pair<std::string, ValueType>;
}
