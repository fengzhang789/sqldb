/* Bison grammar for the query language; the scanner is lexer.l and the entry point wrapping both is parse_statement.
 * Every rule builds the QLNode/QL* tree in ql/ql_ast.h.
 *
 * Ambiguity is resolved by the precedence declarations rather than by one nonterminal per precedence level, which is
 * the point of using bison here: the whole order is the seven %left/%nonassoc/%precedence lines. Bison reports no
 * conflicts (see parser.output in the build directory); the two rules that needed care to keep it that way say so.
 */

%skeleton "lalr1.cc"
%require "3.3"

%define api.namespace {ql}
%define api.parser.class {Parser}
%define api.value.type variant
%define api.token.constructor
%define parse.assert
%define parse.error verbose
%locations

%param { yyscan_t yyscanner }
%param { ql::ParseContext& ctx }

%code requires {
    #include <cstdint>
    #include <string>
    #include <utility>
    #include <vector>

    #include "ql/ql_ast.h"
    #include "ql/ql_driver.h"

    // Spelled out rather than included, so this generated header doesn't depend on the generated lexer one.
    #ifndef YY_TYPEDEF_YY_SCANNER_T
    #define YY_TYPEDEF_YY_SCANNER_T
    typedef void* yyscan_t;
    #endif
}

%code {
    #include <algorithm>

    // Defined by the flex scanner; see YY_DECL in lexer.l.
    ql::Parser::symbol_type yylex(yyscan_t yyscanner, ql::ParseContext& ctx);

    namespace {
        // A row arrives as one expression: a QL_TUP is the row's columns, anything else a single-column row.
        std::vector<QLNode> row_from_expr(QLNode expr) {
            if (expr.type == QL_TUP) return std::move(expr.kids);
            std::vector<QLNode> row;
            row.push_back(std::move(expr));
            return row;
        }

        // Names the item as QLSelect documents: its AS name, else the column name, else its 1-based position.
        void add_select_item(QLSelect* sel, ql::NamedExpr item) {
            std::string name = std::move(item.first);
            if (name.empty()) {
                name = item.second.type == QL_SYM ? item.second.val.str
                                                  : "col" + std::to_string(sel->output.size() + 1);
            }
            sel->names.push_back(std::move(name));
            sel->output.push_back(std::move(item.second));
        }

        // Type names are ordinary identifiers rather than keywords, so `text` stays usable as a column name.
        bool value_type_from_name(const std::string& name, ValueType* out) {
            std::string upper = name;
            std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });
            if (upper == "INT64" || upper == "INT" || upper == "INTEGER" || upper == "BIGINT") {
                *out = INT_64;
                return true;
            }
            if (upper == "BYTES" || upper == "STR" || upper == "STRING" || upper == "TEXT" || upper == "VARCHAR" ||
                upper == "BLOB") {
                *out = BYTES;
                return true;
            }
            return false;
        }

        // TableDef wants the primary key first, so the PRIMARY KEY columns move to the front in the order that clause
        // named them. False with *err set if it names an undeclared column, or one twice.
        bool build_table_def(std::string name, std::vector<ql::ColDef> cols, const std::vector<std::string>& pk,
                             TableDef* out, std::string* err) {
            for (size_t i = 0; i < cols.size(); ++i) {
                for (size_t j = i + 1; j < cols.size(); ++j) {
                    if (cols[i].first == cols[j].first) {
                        *err = "duplicate column: " + cols[i].first;
                        return false;
                    }
                }
            }

            TableDefBuilder builder(std::move(name));
            std::vector<bool> taken(cols.size(), false);
            for (const std::string& key : pk) {
                auto it = std::find_if(cols.begin(), cols.end(),
                                       [&key](const ql::ColDef& col) { return col.first == key; });
                if (it == cols.end()) {
                    *err = "primary key column is not defined: " + key;
                    return false;
                }
                size_t at = static_cast<size_t>(it - cols.begin());
                if (taken[at]) {
                    *err = "duplicate primary key column: " + key;
                    return false;
                }
                taken[at] = true;
                builder.add_col(it->first, it->second);
            }
            for (size_t i = 0; i < cols.size(); ++i) {
                if (!taken[i]) builder.add_col(cols[i].first, cols[i].second);
            }

            *out = builder.set_pkeys(static_cast<int>(pk.size())).build();
            return true;
        }
    }
}

%token END_OF_INPUT 0 "end of input"

%token <int64_t> NUM "integer literal"
%token <std::string> IDENT "identifier"
%token <std::string> STR "string literal"

%token SELECT "SELECT"
%token FROM "FROM"
%token WHERE "WHERE"
%token FILTER "FILTER"
%token LIMIT "LIMIT"
%token INSERT "INSERT"
%token INTO "INTO"
%token VALUES "VALUES"
%token UPDATE "UPDATE"
%token SET "SET"
%token DELETE "DELETE"
%token CREATE "CREATE"
%token TABLE "TABLE"
%token PRIMARY "PRIMARY"
%token KEY "KEY"
%token AS "AS"

%token LPAREN "("
%token RPAREN ")"
%token COMMA ","
%token SEMI ";"
%token AND "AND"
%token OR "OR"
%token NOT "NOT"
%token EQ "="
%token NE "!="
%token LT "<"
%token LE "<="
%token GT ">"
%token GE ">="
%token PLUS "+"
%token MINUS "-"
%token STAR "*"
%token SLASH "/"
%token PERCENT "%"

/* Loosest first: the book's order, minus the tuple level (see `atom`). Comparisons are %nonassoc so `a = b = c` is
 * rejected instead of quietly grouping one way, and the prefix operators take %precedence since associativity means
 * nothing to them. Aliases stay on the %token lines: here a string literal would declare a second, unused token. */
%left OR
%left AND
%precedence NOT
%nonassoc EQ NE LT LE GT GE
%left PLUS MINUS
%left STAR SLASH PERCENT
%precedence UMINUS

%type <QLCreateTable> create_stmt
%type <QLSelect> select_stmt select_list
%type <QLInsert> insert_stmt
%type <QLUpdate> update_stmt set_list
%type <QLDelete> delete_stmt
%type <QLNode> expr atom filter_clause
%type <std::vector<QLNode>> expr_list value_row
%type <std::vector<std::vector<QLNode>>> value_rows
%type <std::vector<std::string>> col_list pk_clause
%type <std::vector<ql::ColDef>> col_defs
%type <ql::ColDef> col_def
%type <ql::NamedExpr> select_item assignment
%type <std::pair<int64_t, int64_t>> limit_clause

%start statement

%%

/* One statement per parse, so there is nothing to resynchronize on: no `error` production, and the first syntax error
 * ends the parse with a located message (see Parser::error). */
statement
    : create_stmt opt_semi { *ctx.out = std::move($1); }
    | select_stmt opt_semi { *ctx.out = std::move($1); }
    | insert_stmt opt_semi { *ctx.out = std::move($1); }
    | update_stmt opt_semi { *ctx.out = std::move($1); }
    | delete_stmt opt_semi { *ctx.out = std::move($1); }
    ;

opt_semi
    : %empty
    | SEMI
    ;

create_stmt
    : CREATE TABLE IDENT LPAREN col_defs COMMA pk_clause RPAREN {
          std::string err;
          if (!build_table_def(std::move($3), std::move($5), $7, &$$.def, &err)) {
              throw syntax_error(@$, err);
          }
      }
    ;

col_defs
    : col_def { $$.push_back(std::move($1)); }
    | col_defs COMMA col_def { $$ = std::move($1); $$.push_back(std::move($3)); }
    ;

col_def
    : IDENT IDENT {
          ValueType type = ERROR;
          if (!value_type_from_name($2, &type)) throw syntax_error(@2, "unknown column type: " + $2);
          $$ = ql::ColDef(std::move($1), type);
      }
    ;

pk_clause
    : PRIMARY KEY LPAREN col_list RPAREN { $$ = std::move($4); }
    ;

col_list
    : IDENT { $$.push_back(std::move($1)); }
    | col_list COMMA IDENT { $$ = std::move($1); $$.push_back(std::move($3)); }
    ;

select_stmt
    : SELECT select_list FROM IDENT filter_clause limit_clause {
          $$ = std::move($2);
          $$.scan.table = std::move($4);
          $$.scan.filter = std::move($5);
          $$.scan.offset = $6.first;
          $$.scan.limit = $6.second;
      }
    ;

select_list
    : select_item { add_select_item(&$$, std::move($1)); }
    | select_list COMMA select_item { $$ = std::move($1); add_select_item(&$$, std::move($3)); }
    ;

select_item
    : STAR { $$ = ql::NamedExpr("*", ql_star()); }
    | expr { $$ = ql::NamedExpr("", std::move($1)); }
    | expr AS IDENT { $$ = ql::NamedExpr(std::move($3), std::move($1)); }
    ;

insert_stmt
    : INSERT INTO IDENT LPAREN col_list RPAREN VALUES value_rows {
          $$.table = std::move($3);
          $$.names = std::move($5);
          $$.values = std::move($8);
      }
    ;

value_rows
    : value_row { $$.push_back(std::move($1)); }
    | value_rows COMMA value_row { $$ = std::move($1); $$.push_back(std::move($3)); }
    ;

/* A row is just a tuple expression. Spelling it out as LPAREN expr_list RPAREN would be ambiguous against the
 * parenthesized expression in `atom` - both start with "(" then an expression - and precedence cannot settle that,
 * since the readings differ in structure, not in binding strength. */
value_row
    : expr { $$ = row_from_expr(std::move($1)); }
    ;

update_stmt
    : UPDATE IDENT SET set_list filter_clause limit_clause {
          $$ = std::move($4);
          $$.scan.table = std::move($2);
          $$.scan.filter = std::move($5);
          $$.scan.offset = $6.first;
          $$.scan.limit = $6.second;
      }
    ;

set_list
    : assignment {
          $$.names.push_back(std::move($1.first));
          $$.values.push_back(std::move($1.second));
      }
    | set_list COMMA assignment {
          $$ = std::move($1);
          $$.names.push_back(std::move($3.first));
          $$.values.push_back(std::move($3.second));
      }
    ;

assignment
    : IDENT EQ expr { $$ = ql::NamedExpr(std::move($1), std::move($3)); }
    ;

delete_stmt
    : DELETE FROM IDENT filter_clause limit_clause {
          $$.scan.table = std::move($3);
          $$.scan.filter = std::move($4);
          $$.scan.offset = $5.first;
          $$.scan.limit = $5.second;
      }
    ;

/* WHERE and FILTER are one clause under two spellings, which keeps the book's FILTER working for a single rule. */
filter_clause
    : %empty { $$ = QLNode(); }
    | where_kw expr { $$ = std::move($2); }
    ;

where_kw
    : WHERE
    | FILTER
    ;

limit_clause
    : %empty { $$ = std::pair<int64_t, int64_t>(0, -1); }
    | LIMIT NUM { $$ = std::pair<int64_t, int64_t>(0, $2); }
    | LIMIT NUM COMMA NUM { $$ = std::pair<int64_t, int64_t>($2, $4); }
    ;

expr
    : expr OR expr { $$ = ql_binop(QL_OR, std::move($1), std::move($3)); }
    | expr AND expr { $$ = ql_binop(QL_AND, std::move($1), std::move($3)); }
    | NOT expr { $$ = ql_unop(QL_NOT, std::move($2)); }
    | expr EQ expr { $$ = ql_binop(QL_CMP_EQ, std::move($1), std::move($3)); }
    | expr NE expr { $$ = ql_binop(QL_CMP_NE, std::move($1), std::move($3)); }
    | expr LT expr { $$ = ql_binop(QL_CMP_LT, std::move($1), std::move($3)); }
    | expr LE expr { $$ = ql_binop(QL_CMP_LE, std::move($1), std::move($3)); }
    | expr GT expr { $$ = ql_binop(QL_CMP_GT, std::move($1), std::move($3)); }
    | expr GE expr { $$ = ql_binop(QL_CMP_GE, std::move($1), std::move($3)); }
    | expr PLUS expr { $$ = ql_binop(QL_ADD, std::move($1), std::move($3)); }
    | expr MINUS expr { $$ = ql_binop(QL_SUB, std::move($1), std::move($3)); }
    | expr STAR expr { $$ = ql_binop(QL_MUL, std::move($1), std::move($3)); }
    | expr SLASH expr { $$ = ql_binop(QL_DIV, std::move($1), std::move($3)); }
    | expr PERCENT expr { $$ = ql_binop(QL_MOD, std::move($1), std::move($3)); }
    | MINUS expr %prec UMINUS { $$ = ql_unop(QL_NEG, std::move($2)); }
    | atom { $$ = std::move($1); }
    ;

/* Tuples are built inside parentheses rather than by making "," an operator at the loosest precedence as the book's
 * level order suggests: as an operator it would swallow the commas separating SELECT items, SET assignments and
 * VALUES rows, an ambiguity in the language itself and so out of reach of a precedence declaration. */
atom
    : NUM { $$ = ql_int64($1); }
    | STR { $$ = ql_str(std::move($1)); }
    | IDENT { $$ = ql_sym(std::move($1)); }
    | LPAREN expr RPAREN { $$ = std::move($2); }
    | LPAREN expr COMMA expr_list RPAREN {
          std::vector<QLNode> kids;
          kids.push_back(std::move($2));
          for (QLNode& kid : $4) {
              kids.push_back(std::move(kid));
          }
          $$ = ql_tuple(std::move(kids));
      }
    ;

expr_list
    : expr { $$.push_back(std::move($1)); }
    | expr_list COMMA expr { $$ = std::move($1); $$.push_back(std::move($3)); }
    ;

%%

// Called for every syntax error, including the ones lexer.l throws.
void ql::Parser::error(const location_type& loc, const std::string& msg) {
    if (!ctx.err.empty()) return; // the first error is the one worth reporting
    ctx.err = std::to_string(loc.begin.line) + ":" + std::to_string(loc.begin.column) + ": " + msg;
}
