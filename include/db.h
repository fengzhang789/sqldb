#pragma once

#include <string>

#include "access/scanner.h"
#include "catalog/catalog.h"
#include "catalog/record.h"
#include "catalog/tabledef.h"
#include "storage/kv.h"

class DB;

// DBTX runs table operations in a single KVTX, so a row write and its index writes commit or roll back together. Begin,
// commit and abort it through DB; if an operation throws, abort rather than commit, since it may be half done.
class DBTX {
    public:
        bool table_new(TableDef def, std::string* err);
        bool get(const std::string& table, Record* rec, std::string* err);
        bool insert(const std::string& table, const Record& rec, std::string* err);
        bool update(const std::string& table, const Record& rec, std::string* err);
        bool upsert(const std::string& table, const Record& rec, std::string* err);
        bool del(const std::string& table, const Record& rec, std::string* err);
        bool scan(const std::string& table, Scanner* req, std::string* err); // req is only usable until the tx ends

    private:
        friend class DB;
        const TableDef* find_table(const std::string& table, std::string* err);
        DB* db_ = nullptr;
        KVTX kv_tx_;
};

// DB: This is what application code actually calls.
class DB {
    public:
        explicit DB(KV* kv) : kv_(kv) {}
        void begin(DBTX* tx);
        void commit(DBTX* tx); // throws on an I/O error, see KV::commit
        void abort(DBTX* tx);

    private:
        friend class DBTX;
        KV* kv_;  // not owned
        Catalog catalog_;
};
