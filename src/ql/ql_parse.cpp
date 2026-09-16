#include "ql/ql_parse.h"

#include <string>
#include <utility>

#include "ql/ql_driver.h"
#include "parser.tab.hh"

// The generated header declares yylex with flex's default signature unless this says otherwise; it has to match the
// one lexer.l defines.
#define YY_DECL ql::Parser::symbol_type yylex(yyscan_t yyscanner, ql::ParseContext& ctx)
#include "lexer.yy.h"

bool parse_statement(std::string_view sql, QLStatement* out, std::string* err) {
    ql::ParseContext ctx;
    QLStatement parsed;
    ctx.out = &parsed;

    yyscan_t scanner = nullptr;
    if (yylex_init_extra(&ctx, &scanner) != 0) {
        *err = "failed to start the lexer";
        return false;
    }
    // yy_scan_bytes copies the input, so sql need not be NUL-terminated or outlive the parse.
    YY_BUFFER_STATE buf = yy_scan_bytes(sql.data(), static_cast<int>(sql.size()), scanner);

    ql::Parser parser(scanner, ctx);
    int rc = parser.parse();

    yy_delete_buffer(buf, scanner);
    yylex_destroy(scanner);

    if (rc != 0) {
        *err = ctx.err.empty() ? "syntax error" : ctx.err;
        return false;
    }
    *out = std::move(parsed);
    return true;
}
