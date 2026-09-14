#pragma once

#include <string>

#include "catalog/catalog.h"
#include "catalog/record.h"
#include "catalog/tabledef.h"
#include "storage/kv.h"

// DB: This is what application code actually calls.
class DB {
    public:
        explicit DB(KV* kv) : kv_(kv), catalog_(kv) {}
        bool table_new(TableDef def, std::string* err);
        bool get(const std::string& table, Record* rec, std::string* err);
        bool insert(const std::string& table, const Record& rec, std::string* err);
        bool update(const std::string& table, const Record& rec, std::string* err);
        bool upsert(const std::string& table, const Record& rec, std::string* err);
        bool del(const std::string& table, const Record& rec, std::string* err);

    private:
        const TableDef* find_table(const std::string& table, std::string* err);
        KV* kv_;  // not owned
        Catalog catalog_;
};
