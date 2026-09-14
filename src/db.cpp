#include "db.h"

#include "access/table_access.h"

const TableDef* DB::find_table(const std::string& table, std::string* err) {
    const TableDef* tdef = catalog_.get_table_def(table);
    if (tdef == nullptr) {
        *err = "table not found: " + table;
    }
    return tdef;
}

bool DB::table_new(TableDef def, std::string* err) {
    return catalog_.table_new(std::move(def), err);
}

bool DB::get(const std::string& table, Record* rec, std::string* err) {
    const TableDef* tdef = find_table(table, err);
    if (tdef == nullptr) return false;
    return db_get(kv_, *tdef, rec, err);
}

bool DB::insert(const std::string& table, const Record& rec, std::string* err) {
    const TableDef* tdef = find_table(table, err);
    if (tdef == nullptr) return false;
    return db_update(kv_, *tdef, rec, UpdateMode::INSERT_ONLY, err);
}

bool DB::update(const std::string& table, const Record& rec, std::string* err) {
    const TableDef* tdef = find_table(table, err);
    if (tdef == nullptr) return false;
    return db_update(kv_, *tdef, rec, UpdateMode::UPDATE_ONLY, err);
}

bool DB::upsert(const std::string& table, const Record& rec, std::string* err) {
    const TableDef* tdef = find_table(table, err);
    if (tdef == nullptr) return false;
    return db_update(kv_, *tdef, rec, UpdateMode::UPSERT, err);
}

bool DB::del(const std::string& table, const Record& rec, std::string* err) {
    const TableDef* tdef = find_table(table, err);
    if (tdef == nullptr) return false;
    return db_delete(kv_, *tdef, rec, err);
}
