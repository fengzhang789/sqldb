/* Flex scanner for the query language; the grammar is parser.y. It is reentrant and takes its position from the
 * ParseContext it is handed, so a parse keeps no global state and two threads can parse at once. Tokens are bison
 * symbol_type values (api.token.constructor), so this has to be compiled as C++.
 */

%{
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <string>

#include "ql/ql_driver.h"
#include "parser.tab.hh"

// Must match the declaration in parser.y; flex builds yylex's signature out of this.
#define YY_DECL ql::Parser::symbol_type yylex(yyscan_t yyscanner, ql::ParseContext& ctx)

// Flex counts lines but not columns, so every rule advances the column and the rules that can span lines fix up the
// line count themselves.
#define YY_USER_ACTION           \
    tok_line = ctx.line;         \
    tok_col = ctx.col;           \
    ctx.col += static_cast<int>(yyleng);

// The span the current token covers, for error messages.
#define TOK_LOC \
    ql::location(ql::position(nullptr, tok_line, tok_col), ql::position(nullptr, ctx.line, ctx.col))

namespace {
    // Decodes a literal's body: '' is one quote, and a backslash escapes the character after it.
    std::string unquote(const char* text, int len) {
        std::string out;
        for (int i = 1; i + 1 < len; ++i) {
            char c = text[i];
            if (c == '\'' && text[i + 1] == '\'') {
                out += '\'';
                ++i;
                continue;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            switch (text[++i]) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '0': out += '\0'; break;
                default: out += text[i]; break; // \\ and \' land here too
            }
        }
        return out;
    }

    // Keeps the count right across a token holding newlines, i.e. a block comment.
    void count_lines(ql::ParseContext* ctx, const char* text, int len) {
        int last_nl = -1;
        for (int i = 0; i < len; ++i) {
            if (text[i] == '\n') {
                ++ctx->line;
                last_nl = i;
            }
        }
        if (last_nl >= 0) ctx->col = len - last_nl;
    }
}
%}

%option reentrant
%option caseless
%option noyywrap nounput noinput never-interactive
%option extra-type="ql::ParseContext *"

%%

%{
    // Declared at the top of yylex, where YY_USER_ACTION can reach them.
    int tok_line = ctx.line;
    int tok_col = ctx.col;
%}

[ \t\r]+                   { }
\n                         { ++ctx.line; ctx.col = 1; }
"--"[^\n]*                 { }
"/*"([^*]|\*+[^*/])*\*+"/" { count_lines(&ctx, yytext, static_cast<int>(yyleng)); }

"select"    { return ql::Parser::make_SELECT(TOK_LOC); }
"from"      { return ql::Parser::make_FROM(TOK_LOC); }
"where"     { return ql::Parser::make_WHERE(TOK_LOC); }
"filter"    { return ql::Parser::make_FILTER(TOK_LOC); }
"limit"     { return ql::Parser::make_LIMIT(TOK_LOC); }
"insert"    { return ql::Parser::make_INSERT(TOK_LOC); }
"into"      { return ql::Parser::make_INTO(TOK_LOC); }
"values"    { return ql::Parser::make_VALUES(TOK_LOC); }
"update"    { return ql::Parser::make_UPDATE(TOK_LOC); }
"set"       { return ql::Parser::make_SET(TOK_LOC); }
"delete"    { return ql::Parser::make_DELETE(TOK_LOC); }
"create"    { return ql::Parser::make_CREATE(TOK_LOC); }
"table"     { return ql::Parser::make_TABLE(TOK_LOC); }
"primary"   { return ql::Parser::make_PRIMARY(TOK_LOC); }
"key"       { return ql::Parser::make_KEY(TOK_LOC); }
"as"        { return ql::Parser::make_AS(TOK_LOC); }
"and"       { return ql::Parser::make_AND(TOK_LOC); }
"or"        { return ql::Parser::make_OR(TOK_LOC); }
"not"       { return ql::Parser::make_NOT(TOK_LOC); }

">="        { return ql::Parser::make_GE(TOK_LOC); }
"<="        { return ql::Parser::make_LE(TOK_LOC); }
"!="        { return ql::Parser::make_NE(TOK_LOC); }
"<>"        { return ql::Parser::make_NE(TOK_LOC); }
"="         { return ql::Parser::make_EQ(TOK_LOC); }
"<"         { return ql::Parser::make_LT(TOK_LOC); }
">"         { return ql::Parser::make_GT(TOK_LOC); }
"+"         { return ql::Parser::make_PLUS(TOK_LOC); }
"-"         { return ql::Parser::make_MINUS(TOK_LOC); }
"*"         { return ql::Parser::make_STAR(TOK_LOC); }
"/"         { return ql::Parser::make_SLASH(TOK_LOC); }
"%"         { return ql::Parser::make_PERCENT(TOK_LOC); }
"("         { return ql::Parser::make_LPAREN(TOK_LOC); }
")"         { return ql::Parser::make_RPAREN(TOK_LOC); }
","         { return ql::Parser::make_COMMA(TOK_LOC); }
";"         { return ql::Parser::make_SEMI(TOK_LOC); }

[0-9]+ {
    errno = 0;
    char* end = nullptr;
    long long v = std::strtoll(yytext, &end, 10);
    if (errno == ERANGE) {
        throw ql::Parser::syntax_error(TOK_LOC, "integer literal out of range: " + std::string(yytext));
    }
    return ql::Parser::make_NUM(static_cast<int64_t>(v), TOK_LOC);
}

[A-Za-z_][A-Za-z0-9_]* { return ql::Parser::make_IDENT(std::string(yytext, yyleng), TOK_LOC); }

'([^'\\\n]|\\.|'')*' { return ql::Parser::make_STR(unquote(yytext, static_cast<int>(yyleng)), TOK_LOC); }

    /* Only reachable when the rule above didn't match, i.e. the quote is never closed. */
' { throw ql::Parser::syntax_error(TOK_LOC, "unterminated string literal"); }

<<EOF>> { return ql::Parser::make_END_OF_INPUT(TOK_LOC); }

. { throw ql::Parser::syntax_error(TOK_LOC, "unexpected character: " + std::string(yytext, 1)); }

%%
