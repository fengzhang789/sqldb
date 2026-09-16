#include "db.h"

#include "access/table_access.h"

void DB::begin_read(DBReader* tx) {
    tx->db_ = this;
    tx->tables_.clear();
    kv_->begin_read(&tx->kv_reader_);
}

void DB::end_read(DBReader* tx) {
    kv_->end_read(&tx->kv_reader_);
}

void DB::begin(DBTX* tx) {
    tx->db_ = this;
    tx->created_table_ = false;
    kv_->begin(&tx->kv_tx_);
}

// The catalog cache must only hold committed defs, but tx may have cached one for a table it created. It is dropped
// before the commit rather than after a failed one, since by then the writer lock that guards the cache is released.
void DB::commit(DBTX* tx) {
    if (tx->created_table_) {
        catalog_.clear_cache();
    }
    kv_->commit(&tx->kv_tx_);
}

void DB::abort(DBTX* tx) {
    if (tx->created_table_) {
        catalog_.clear_cache(); // while the writer lock is still held, as in commit
    }
    kv_->abort(&tx->kv_tx_);
}

// Readers skip the catalog cache, which only write transactions may touch; each def lives as long as the reader.
const TableDef* DBReader::find_table(const std::string& table, std::string* err) {
    auto it = tables_.find(table);
    if (it == tables_.end()) {
        std::unique_ptr<TableDef> tdef = db_->catalog_.get_table_def_from_kv(&kv_reader_, table);
        if (!tdef) {
            *err = "table not found: " + table;
            return nullptr;
        }
        it = tables_.emplace(table, std::move(tdef)).first;
    }
    return it->second.get();
}

bool DBReader::get(const std::string& table, Record* rec, std::string* err) {
    const TableDef* tdef = find_table(table, err);
    if (tdef == nullptr) return false;
    return db_get(&kv_reader_, *tdef, rec, err);
}

bool DBReader::scan(const std::string& table, Scanner* req, std::string* err) {
    const TableDef* tdef = find_table(table, err);
    if (tdef == nullptr) return false;
    return db_scan(&kv_reader_, *tdef, req, err);
}

const TableDef* DBReader::table_def(const std::string& table, std::string* err) {
    return find_table(table, err);
}

const TableDef* DBTX::find_table(const std::string& table, std::string* err) {
    const TableDef* tdef = db_->catalog_.get_table_def(&kv_tx_, table);
    if (tdef == nullptr) {
        *err = "table not found: " + table;
    }
    return tdef;
}

bool DBTX::table_new(TableDef def, std::string* err) {
    if (!db_->catalog_.table_new(&kv_tx_, std::move(def), err)) return false;
    created_table_ = true;
    return true;
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

const TableDef* DBTX::table_def(const std::string& table, std::string* err) {
    return find_table(table, err);
}
