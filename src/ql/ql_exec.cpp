#include "ql/ql_exec.h"

#include <limits>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "access/scanner.h"
#include "catalog/catalog.h"
#include "ql/ql_parse.h"
#include "ql/ql_range.h"

namespace {
    constexpr int64_t I64_MIN = std::numeric_limits<int64_t>::min();

    bool eval_kids(const QLNode& node, const Record& env, Value* lhs, Value* rhs, std::string* err) {
        return ql_eval(node.kids[0], env, lhs, err) && ql_eval(node.kids[1], env, rhs, err);
    }

    Value make_bool(bool v) {
        return Value::make_int64(v ? 1 : 0);
    }

    // -1, 0 or 1 for lhs against rhs; strings order lexicographically.
    bool compare(const Value& lhs, const Value& rhs, int* out, std::string* err) {
        if (lhs.type != rhs.type) {
            *err = "cannot compare a number with a string";
            return false;
        }
        if (lhs.type == INT_64) {
            *out = lhs.int64 < rhs.int64 ? -1 : (lhs.int64 > rhs.int64 ? 1 : 0);
        } else {
            int c = lhs.str.compare(rhs.str);
            *out = c < 0 ? -1 : (c > 0 ? 1 : 0);
        }
        return true;
    }

    bool cmp_holds(QLNodeType type, int c) {
        switch (type) {
            case QL_CMP_EQ: return c == 0;
            case QL_CMP_NE: return c != 0;
            case QL_CMP_LT: return c < 0;
            case QL_CMP_LE: return c <= 0;
            case QL_CMP_GT: return c > 0;
            default: return c >= 0; // QL_CMP_GE
        }
    }

    // Integer arithmetic, rejecting the cases that would otherwise be undefined behaviour.
    bool arith(QLNodeType type, int64_t a, int64_t b, int64_t* out, std::string* err) {
        bool overflow = false;
        switch (type) {
            case QL_ADD: overflow = __builtin_add_overflow(a, b, out); break;
            case QL_SUB: overflow = __builtin_sub_overflow(a, b, out); break;
            case QL_MUL: overflow = __builtin_mul_overflow(a, b, out); break;
            default:
                if (b == 0) {
                    *err = type == QL_DIV ? "division by zero" : "modulo by zero";
                    return false;
                }
                if (a == I64_MIN && b == -1) {
                    overflow = true;
                    break;
                }
                *out = type == QL_DIV ? a / b : a % b;
                break;
        }
        if (overflow) *err = "integer overflow";
        return !overflow;
    }

    // Every row of the scan's table, filtered and limited. Templated so a DBReader and a DBTX both drive it.
    template <typename TX>
    bool scan_rows(const QLScan& req, TX* tx, std::vector<Record>* out, std::string* err) {
        if (req.key1.type != QL_UNINIT || req.key2.type != QL_UNINIT) {
            *err = "INDEX BY is not supported; this query language filters with WHERE";
            return false;
        }

        const TableDef* tdef = tx->table_def(req.table, err);
        if (tdef == nullptr) return false;

        // The range narrows what is read; the filter below still runs on every row it returns.
        QLRange range = ql_range(req.filter, *tdef);
        Scanner sc(range.cmp1, range.cmp2, std::move(range.key1), std::move(range.key2));
        if (!tx->scan(req.table, &sc, err)) return false;

        int64_t skipped = 0;
        int64_t taken = 0;
        for (; sc.valid(); sc.next()) {
            Record rec;
            sc.deref(&rec);

            if (req.filter.type != QL_UNINIT) {
                Value keep;
                if (!ql_eval(req.filter, rec, &keep, err)) return false;
                if (keep.type != INT_64) {
                    *err = "WHERE is not a boolean";
                    return false;
                }
                if (keep.int64 == 0) continue;
            }
            // WHERE first, so OFFSET and LIMIT count matching rows as SQL does, not scanned ones as the book does.
            if (skipped < req.offset) {
                ++skipped;
                continue;
            }
            if (req.limit >= 0 && taken >= req.limit) break;
            out->push_back(std::move(rec));
            ++taken;
        }
        return true;
    }

    template <typename TX>
    bool select_rows(const QLSelect& req, TX* tx, std::vector<Record>* out, std::string* err) {
        std::vector<Record> rows;
        if (!scan_rows(req.scan, tx, &rows, err)) return false;

        for (const Record& row : rows) {
            Record orec;
            for (size_t i = 0; i < req.output.size(); ++i) {
                if (req.output[i].type == QL_STAR) {
                    orec.cols.insert(orec.cols.end(), row.cols.begin(), row.cols.end());
                    orec.vals.insert(orec.vals.end(), row.vals.begin(), row.vals.end());
                    continue;
                }
                Value v;
                if (!ql_eval(req.output[i], row, &v, err)) return false;
                orec.cols.push_back(req.names[i]);
                orec.vals.push_back(std::move(v));
            }
            out->push_back(std::move(orec));
        }
        return true;
    }

    // The scanned row's primary key, as db_delete wants it: exactly the pk columns.
    bool primary_key(const Record& row, const std::vector<std::string>& pk, Record* out, std::string* err) {
        for (const std::string& col : pk) {
            const Value* v = row.get(col);
            if (v == nullptr) {
                *err = "row is missing primary key column: " + col;
                return false;
            }
            out->cols.push_back(col);
            out->vals.push_back(*v);
        }
        return true;
    }
}

bool ql_eval(const QLNode& node, const Record& env, Value* out, std::string* err) {
    switch (node.type) {
        case QL_I64:
        case QL_STR:
            *out = node.val;
            return true;

        case QL_SYM: {
            const Value* v = env.get(node.val.str);
            if (v == nullptr) {
                *err = "unknown column: " + node.val.str;
                return false;
            }
            *out = *v;
            return true;
        }

        case QL_NEG: {
            Value v;
            if (!ql_eval(node.kids[0], env, &v, err)) return false;
            if (v.type != INT_64) {
                *err = "unary minus needs a number";
                return false;
            }
            int64_t r = 0;
            if (!arith(QL_SUB, 0, v.int64, &r, err)) return false; // catches -INT64_MIN
            *out = Value::make_int64(r);
            return true;
        }

        case QL_NOT: {
            Value v;
            if (!ql_eval(node.kids[0], env, &v, err)) return false;
            if (v.type != INT_64) {
                *err = "NOT needs a boolean";
                return false;
            }
            *out = make_bool(v.int64 == 0);
            return true;
        }

        case QL_AND:
        case QL_OR: {
            Value a;
            Value b;
            if (!eval_kids(node, env, &a, &b, err)) return false;
            if (a.type != INT_64 || b.type != INT_64) {
                *err = std::string(node.type == QL_AND ? "AND" : "OR") + " needs booleans";
                return false;
            }
            bool lhs = a.int64 != 0;
            bool rhs = b.int64 != 0;
            *out = make_bool(node.type == QL_AND ? (lhs && rhs) : (lhs || rhs));
            return true;
        }

        case QL_CMP_EQ:
        case QL_CMP_NE:
        case QL_CMP_LT:
        case QL_CMP_LE:
        case QL_CMP_GT:
        case QL_CMP_GE: {
            Value a;
            Value b;
            if (!eval_kids(node, env, &a, &b, err)) return false;
            int c = 0;
            if (!compare(a, b, &c, err)) return false;
            *out = make_bool(cmp_holds(node.type, c));
            return true;
        }

        case QL_ADD:
        case QL_SUB:
        case QL_MUL:
        case QL_DIV:
        case QL_MOD: {
            Value a;
            Value b;
            if (!eval_kids(node, env, &a, &b, err)) return false;
            if (a.type != INT_64 || b.type != INT_64) {
                *err = "arithmetic needs numbers";
                return false;
            }
            int64_t r = 0;
            if (!arith(node.type, a.int64, b.int64, &r, err)) return false;
            *out = Value::make_int64(r);
            return true;
        }

        case QL_TUP:
            *err = "a tuple is not a value";
            return false;
        case QL_STAR:
            *err = "* is only valid in a SELECT list";
            return false;
        default:
            *err = "empty expression";
            return false;
    }
}

bool ql_scan(const QLScan& req, DBReader* tx, std::vector<Record>* out, std::string* err) {
    return scan_rows(req, tx, out, err);
}

bool ql_scan(const QLScan& req, DBTX* tx, std::vector<Record>* out, std::string* err) {
    return scan_rows(req, tx, out, err);
}

bool ql_select(const QLSelect& req, DBReader* tx, std::vector<Record>* out, std::string* err) {
    return select_rows(req, tx, out, err);
}

bool ql_select(const QLSelect& req, DBTX* tx, std::vector<Record>* out, std::string* err) {
    return select_rows(req, tx, out, err);
}

bool ql_create_table(const QLCreateTable& req, DBTX* tx, std::string* err) {
    return tx->table_new(req.def, err);
}

bool ql_insert(const QLInsert& req, DBTX* tx, uint64_t* count, std::string* err) {
    *count = 0;
    for (const std::vector<QLNode>& values : req.values) {
        if (values.size() != req.names.size()) {
            *err = "row has " + std::to_string(values.size()) + " values but " +
                   std::to_string(req.names.size()) + " columns were named";
            return false;
        }

        Record rec;
        for (size_t i = 0; i < values.size(); ++i) {
            Value v;
            if (!ql_eval(values[i], Record{}, &v, err)) return false; // no row is in scope
            rec.cols.push_back(req.names[i]);
            rec.vals.push_back(std::move(v));
        }

        bool ok = false;
        switch (req.mode) {
            case UpdateMode::INSERT_ONLY: ok = tx->insert(req.table, rec, err); break;
            case UpdateMode::UPDATE_ONLY: ok = tx->update(req.table, rec, err); break;
            case UpdateMode::UPSERT: ok = tx->upsert(req.table, rec, err); break;
        }
        if (!ok) return false;
        ++*count;
    }
    return true;
}

bool ql_update(const QLUpdate& req, DBTX* tx, uint64_t* count, std::string* err) {
    const TableDef* tdef = tx->table_def(req.scan.table, err);
    if (tdef == nullptr) return false;

    // Moving a row would change its key and every index key, so a primary key is replaced by a DELETE and an INSERT.
    for (const std::string& name : req.names) {
        int at = col_index(*tdef, name);
        if (at < 0) {
            *err = "unknown column: " + name;
            return false;
        }
        if (at < tdef->pkeys) {
            *err = "cannot update a primary key column: " + name;
            return false;
        }
    }

    std::vector<Record> rows;
    if (!scan_rows(req.scan, tx, &rows, err)) return false; // collected first: a scan cannot outlive a write

    *count = 0;
    for (Record& row : rows) {
        std::vector<Value> next; // every assignment sees the row as it was, so `SET a = b, b = a` swaps
        for (const QLNode& expr : req.values) {
            Value v;
            if (!ql_eval(expr, row, &v, err)) return false;
            next.push_back(std::move(v));
        }
        for (size_t i = 0; i < req.names.size(); ++i) {
            for (size_t j = 0; j < row.cols.size(); ++j) {
                if (row.cols[j] == req.names[i]) {
                    row.vals[j] = std::move(next[i]);
                    break;
                }
            }
        }
        if (!tx->update(req.scan.table, row, err)) return false;
        ++*count;
    }
    return true;
}

bool ql_delete(const QLDelete& req, DBTX* tx, uint64_t* count, std::string* err) {
    const TableDef* tdef = tx->table_def(req.scan.table, err);
    if (tdef == nullptr) return false;
    std::vector<std::string> pk(tdef->cols.begin(), tdef->cols.begin() + tdef->pkeys);

    std::vector<Record> rows;
    if (!scan_rows(req.scan, tx, &rows, err)) return false;

    *count = 0;
    for (const Record& row : rows) {
        Record key;
        if (!primary_key(row, pk, &key, err)) return false;
        if (!tx->del(req.scan.table, key, err)) return false;
        ++*count;
    }
    return true;
}

bool ql_exec(const QLStatement& stmt, DBTX* tx, QLResult* out, std::string* err) {
    *out = QLResult{};
    if (const auto* s = std::get_if<QLCreateTable>(&stmt)) return ql_create_table(*s, tx, err);
    if (const auto* s = std::get_if<QLSelect>(&stmt)) return ql_select(*s, tx, &out->rows, err);
    if (const auto* s = std::get_if<QLInsert>(&stmt)) return ql_insert(*s, tx, &out->count, err);
    if (const auto* s = std::get_if<QLUpdate>(&stmt)) return ql_update(*s, tx, &out->count, err);
    return ql_delete(std::get<QLDelete>(stmt), tx, &out->count, err);
}

bool ql_exec(const QLStatement& stmt, DBReader* tx, QLResult* out, std::string* err) {
    *out = QLResult{};
    const auto* s = std::get_if<QLSelect>(&stmt);
    if (s == nullptr) {
        *err = "only SELECT can run on a reader; this statement needs a write transaction";
        return false;
    }
    return ql_select(*s, tx, &out->rows, err);
}

bool ql_run(std::string_view sql, DBTX* tx, QLResult* out, std::string* err) {
    QLStatement stmt;
    return parse_statement(sql, &stmt, err) && ql_exec(stmt, tx, out, err);
}

bool ql_run(std::string_view sql, DBReader* tx, QLResult* out, std::string* err) {
    QLStatement stmt;
    return parse_statement(sql, &stmt, err) && ql_exec(stmt, tx, out, err);
}
