#include "access/index_ops.h"

#include <cassert>
#include <string>
#include <vector>

#include "access/row_codec.h"

void index_op(KVTX* tx, const TableDef& tdef, const Record& rec, int op) {
    assert(tdef.index_prefixes.size() == tdef.indexes.size());
    for (size_t i = 0; i < tdef.indexes.size(); ++i) {
        std::vector<Value> index_values;
        for (const std::string& col : tdef.indexes[i]) {
            const Value* v = rec.get(col);
            assert(v != nullptr);
            index_values.push_back(*v);
        }
        std::string key = encode_key(tdef.index_prefixes[i], index_values);

        [[maybe_unused]] bool done = false;
        switch (op) {
            case INDEX_ADD: {
                InsertReq req;
                req.key.assign(key.begin(), key.end()); // the index key is the whole entry; its value stays empty
                req.mode = UpdateMode::INSERT_ONLY;
                done = tx->update(&req);
                break;
            }
            case INDEX_DEL:
                done = tx->del(std::vector<uint8_t>(key.begin(), key.end()));
                break;
            default:
                assert(false && "index_op: bad op");
        }
        assert(done);
    }
}
