#include "ql/ql_range.h"

#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "catalog/catalog.h" // col_index
#include "ql/ql_exec.h" // ql_eval

namespace {
    // What the AND-ed comparisons pin down for one column.
    struct ColBound {
        bool has_eq = false;
        bool has_lo = false;
        bool has_hi = false;
        bool lo_open = false; // > rather than >=
        bool hi_open = false; // < rather than <=
        Value eq;
        Value lo;
        Value hi;
    };

    using Bounds = std::map<std::string, ColBound>;

    bool value_less(const Value& a, const Value& b) {
        return a.type == INT_64 ? a.int64 < b.int64 : a.str < b.str;
    }

    // The operator with its sides swapped, so `5 < a` reads as `a > 5`.
    QLNodeType flip(QLNodeType type) {
        switch (type) {
            case QL_CMP_LT: return QL_CMP_GT;
            case QL_CMP_LE: return QL_CMP_GE;
            case QL_CMP_GT: return QL_CMP_LT;
            case QL_CMP_GE: return QL_CMP_LE;
            default: return type; // = is symmetric
        }
    }

    bool is_comparison(QLNodeType type) {
        return type == QL_CMP_EQ || type == QL_CMP_LT || type == QL_CMP_LE || type == QL_CMP_GT ||
               type == QL_CMP_GE;
    }

    void add_bound(QLNodeType op, const std::string& col, Value v, Bounds* out) {
        ColBound& b = (*out)[col];
        switch (op) {
            case QL_CMP_EQ:
                if (!b.has_eq) {
                    b.has_eq = true;
                    b.eq = std::move(v);
                }
                break;
            case QL_CMP_GT:
            case QL_CMP_GE:
                if (!b.has_lo || value_less(b.lo, v)) { // the tighter of the two still holds every matching row
                    b.has_lo = true;
                    b.lo_open = op == QL_CMP_GT;
                    b.lo = std::move(v);
                }
                break;
            case QL_CMP_LT:
            case QL_CMP_LE:
                if (!b.has_hi || value_less(v, b.hi)) {
                    b.has_hi = true;
                    b.hi_open = op == QL_CMP_LT;
                    b.hi = std::move(v);
                }
                break;
            default:
                break;
        }
    }

    // Collects the comparisons the filter ANDs together. Anything under OR or NOT is ignored: it cannot narrow a
    // range without dropping rows the filter would have kept.
    void collect(const QLNode& node, const TableDef& tdef, Bounds* out) {
        if (node.type == QL_AND) {
            collect(node.kids[0], tdef, out);
            collect(node.kids[1], tdef, out);
            return;
        }
        if (!is_comparison(node.type)) return;

        QLNodeType op = node.type;
        const QLNode* sym = &node.kids[0];
        const QLNode* val = &node.kids[1];
        if (sym->type != QL_SYM) {
            std::swap(sym, val);
            op = flip(op);
        }
        if (sym->type != QL_SYM) return;

        int at = col_index(tdef, sym->val.str);
        if (at < 0) return;

        Value v; // a constant is whatever evaluates against no row at all
        std::string err;
        if (!ql_eval(*val, Record{}, &v, &err)) return;
        if (v.type != tdef.types[at]) return; // the filter rejects every row anyway

        add_bound(op, sym->val.str, std::move(v), out);
    }

    // How much of `cols` the bounds pin down: a run of equality columns, then at most one range column.
    struct Fit {
        size_t eq = 0;
        bool range = false;

        int score() const { return static_cast<int>(eq) * 2 + (range ? 1 : 0); }
    };

    Fit fit_cols(std::span<const std::string> cols, const Bounds& bounds) {
        Fit f;
        while (f.eq < cols.size()) {
            auto it = bounds.find(cols[f.eq]);
            if (it == bounds.end() || !it->second.has_eq) break;
            ++f.eq;
        }
        if (f.eq < cols.size()) {
            auto it = bounds.find(cols[f.eq]);
            f.range = it != bounds.end() && (it->second.has_lo || it->second.has_hi);
        }
        return f;
    }
}

QLRange ql_range(const QLNode& filter, const TableDef& tdef) {
    QLRange out; // empty keys: the whole table, in primary-key order
    if (filter.type == QL_UNINIT) return out;

    Bounds bounds;
    collect(filter, tdef, &bounds);
    if (bounds.empty()) return out;

    std::span<const std::string> best = std::span(tdef.cols).first(static_cast<size_t>(tdef.pkeys));
    Fit best_fit = fit_cols(best, bounds);
    bool best_is_pk = true;
    for (const std::vector<std::string>& index : tdef.indexes) {
        Fit f = fit_cols(index, bounds);
        if (f.score() > best_fit.score()) { // a tie keeps the primary key, whose rows need no second lookup
            best_fit = f;
            best = index;
            best_is_pk = false;
        }
    }
    if (best_fit.score() == 0) return out;

    for (size_t i = 0; i < best_fit.eq; ++i) {
        const ColBound& b = bounds.at(best[i]);
        out.key1.cols.push_back(best[i]);
        out.key1.vals.push_back(b.eq);
        out.key2.cols.push_back(best[i]);
        out.key2.vals.push_back(b.eq);
    }
    if (best_fit.range) {
        const std::string& col = best[best_fit.eq];
        const ColBound& b = bounds.at(col);
        if (b.has_lo) {
            out.key1.cols.push_back(col);
            out.key1.vals.push_back(b.lo);
            out.cmp1 = b.lo_open ? CMP_GT : CMP_GE;
        }
        if (b.has_hi) {
            out.key2.cols.push_back(col);
            out.key2.vals.push_back(b.hi);
            out.cmp2 = b.hi_open ? CMP_LT : CMP_LE;
        }
    }

    // find_index reads the index off key1, so an empty key1 always selects the primary key; a bound only on a
    // secondary index's column would then be checked against the wrong tree.
    if (out.key1.cols.empty() && !best_is_pk) return QLRange{};
    return out;
}
