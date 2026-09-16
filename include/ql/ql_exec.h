#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "catalog/record.h"
#include "catalog/value.h"
#include "db.h"
#include "ql/ql_ast.h"

// What a statement produced: rows for SELECT, a row count for INSERT/UPDATE/DELETE.
struct QLResult {
    std::vector<Record> rows;
    uint64_t count = 0;
};

// Evaluates one expression against a row, which is empty for INSERT values. Booleans are INT_64 0 or 1. False with
// *err set on an unknown column, a type mismatch, division by zero or integer overflow.
bool ql_eval(const QLNode& node, const Record& env, Value* out, std::string* err);

// Appends the rows a scan selects, in primary-key order, applying WHERE and then LIMIT.
bool ql_scan(const QLScan& req, DBReader* tx, std::vector<Record>* out, std::string* err);
bool ql_scan(const QLScan& req, DBTX* tx, std::vector<Record>* out, std::string* err);

// Appends one output record per selected row.
bool ql_select(const QLSelect& req, DBReader* tx, std::vector<Record>* out, std::string* err);
bool ql_select(const QLSelect& req, DBTX* tx, std::vector<Record>* out, std::string* err);

bool ql_create_table(const QLCreateTable& req, DBTX* tx, std::string* err);
bool ql_insert(const QLInsert& req, DBTX* tx, uint64_t* count, std::string* err);
bool ql_update(const QLUpdate& req, DBTX* tx, uint64_t* count, std::string* err);
bool ql_delete(const QLDelete& req, DBTX* tx, uint64_t* count, std::string* err);

// Runs one parsed statement, replacing *out. The DBReader form takes SELECT only.
bool ql_exec(const QLStatement& stmt, DBTX* tx, QLResult* out, std::string* err);
bool ql_exec(const QLStatement& stmt, DBReader* tx, QLResult* out, std::string* err);

// Parses and runs one statement. Neither form begins or ends the transaction.
bool ql_run(std::string_view sql, DBTX* tx, QLResult* out, std::string* err);
bool ql_run(std::string_view sql, DBReader* tx, QLResult* out, std::string* err);
