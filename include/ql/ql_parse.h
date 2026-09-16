#pragma once

#include <string>
#include <string_view>

#include "ql/ql_ast.h"

// Parses one statement, with an optional trailing `;`, into *out. False with *err set to a "line:col: reason" message
// if the input doesn't lex or parse, leaving *out untouched.
//
// The supported subset, with `[]` marking an optional clause:
//     CREATE TABLE name (col type, ..., PRIMARY KEY (col, ...));
//     SELECT expr [AS name], ... FROM table [WHERE cond] [LIMIT [offset,] count];
//     INSERT INTO table (col, ...) VALUES (expr, ...), ...;
//     UPDATE table SET col = expr, ... [WHERE cond] [LIMIT [offset,] count];
//     DELETE FROM table [WHERE cond] [LIMIT [offset,] count];
// FILTER is accepted as a spelling of WHERE. Unlike the book, there is no INDEX BY clause: WHERE covers both the
// indexed and non-indexed cases, leaving it to the executor to decide which index (if any) a condition can use.
//
// Thread-safe: no parser state outlives the call.
bool parse_statement(std::string_view sql, QLStatement* out, std::string* err);
