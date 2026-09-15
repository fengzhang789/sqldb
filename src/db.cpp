#include "db.h"

#include "access/table_access.h"

void DB::begin(DBTX* tx) {
    tx->db_ = this;
    kv_->begin(&tx->kv_tx_);
}

void DB::commit(DBTX* tx) {
    try {
        kv_->commit(&tx->kv_tx_);
    } catch (...) {
        catalog_.clear_cache(); // the commit may have rolled back tables the cache read through tx
        throw;
    }
}

void DB::abort(DBTX* tx) {
    kv_->abort(&tx->kv_tx_);
    catalog_.clear_cache(); // it may hold tables that only existed in tx
}

const TableDef* DBTX::find_table(const std::string& table, std::string* err) {
    const TableDef* tdef = db_->catalog_.get_table_def(&kv_tx_, table);
    if (tdef == nullptr) {
        *err = "table not found: " + table;
    }
    return tdef;
}

bool DBTX::table_new(TableDef def, std::string* err) {
    return db_->catalog_.table_new(&kv_tx_, std::move(def), err);
}

bool DBTX::get(const std::string& table, Record* rec, std::string* err) {
    const TableDef* tdef = find_table(table, err);
    if (tdef == nullptr) return false;
    return db_get(&kv_tx_, *tdef, rec, err);
}

bool DBTX::insert(const std::string& table, const Record& rec, std::string* err) {
    const TableDef* tdef = find_table(table, err);
    if (tdef == nullptr) return false;
    return db_update(&kv_tx_, *tdef, rec, UpdateMode::INSERT_ONLY, err);
}

bool DBTX::update(const std::string& table, const Record& rec, std::string* err) {
    const TableDef* tdef = find_table(table, err);
    if (tdef == nullptr) return false;
    return db_update(&kv_tx_, *tdef, rec, UpdateMode::UPDATE_ONLY, err);
}

bool DBTX::upsert(const std::string& table, const Record& rec, std::string* err) {
    const TableDef* tdef = find_table(table, err);
    if (tdef == nullptr) return false;
    return db_update(&kv_tx_, *tdef, rec, UpdateMode::UPSERT, err);
}

bool DBTX::del(const std::string& table, const Record& rec, std::string* err) {
    const TableDef* tdef = find_table(table, err);
    if (tdef == nullptr) return false;
    return db_delete(&kv_tx_, *tdef, rec, err);
}

bool DBTX::scan(const std::string& table, Scanner* req, std::string* err) {
    const TableDef* tdef = find_table(table, err);
    if (tdef == nullptr) return false;
    return db_scan(&kv_tx_, *tdef, req, err);
}
