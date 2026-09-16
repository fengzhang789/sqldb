#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "access/scanner.h"
#include "catalog/catalog.h"
#include "catalog/record.h"
#include "catalog/tabledef.h"
#include "storage/kv.h"

class DB;

// DBReader runs read-only table operations on one KVReader snapshot, alongside other readers and a DBTX. Begin and end
// it through DB.
class DBReader {
    public:
        bool get(const std::string& table, Record* rec, std::string* err);
        bool scan(const std::string& table, Scanner* req, std::string* err); // req is only usable until the reader ends

    private:
        friend class DB;
        const TableDef* find_table(const std::string& table, std::string* err);
        DB* db_ = nullptr;
        KVReader kv_reader_;
        std::unordered_map<std::string, std::unique_ptr<TableDef>> tables_; // defs read so far, owned by the reader
};

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
        bool created_table_ = false; // the catalog cache may hold a def this tx wrote, see DB::commit
};

// DB: This is what application code actually calls.
class DB {
    public:
        explicit DB(KV* kv) : kv_(kv) {}
        void begin_read(DBReader* tx);
        void end_read(DBReader* tx);
        void begin(DBTX* tx);
        void commit(DBTX* tx); // throws on an I/O error, see KV::commit
        void abort(DBTX* tx);

    private:
        friend class DBReader;
        friend class DBTX;
        KV* kv_;  // not owned
        Catalog catalog_; // only write transactions use its cache
};
