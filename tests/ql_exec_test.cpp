#include "ql/ql_exec.h"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "access/scanner.h"
#include "catalog/tabledef.h"
#include "ql/ql_parse.h"

namespace {
    constexpr int64_t I64_MIN = std::numeric_limits<int64_t>::min();
    constexpr int64_t I64_MAX = std::numeric_limits<int64_t>::max();

    using Rows = std::vector<std::string>;

    // "id=1 name=ann", so a row set reads as one line in a failure message.
    std::string to_text(const Record& rec) {
        std::string out;
        for (size_t i = 0; i < rec.cols.size(); ++i) {
            if (i > 0) out += " ";
            out += rec.cols[i] + "=";
            out += rec.vals[i].type == INT_64 ? std::to_string(rec.vals[i].int64) : rec.vals[i].str;
        }
        return out;
    }

    Rows to_text(const std::vector<Record>& rows) {
        Rows out;
        for (const Record& rec : rows) {
            out.push_back(to_text(rec));
        }
        return out;
    }

    Record age_key(int64_t age) {
        Record rec;
        rec.add_int64("age", age);
        return rec;
    }

    // Evaluates an expression by parsing it as a SELECT item.
    bool eval(const std::string& expr, const Record& env, Value* out, std::string* err) {
        QLStatement stmt;
        if (!parse_statement("SELECT " + expr + " FROM t", &stmt, err)) return false;
        return ql_eval(std::get<QLSelect>(stmt).output[0], env, out, err);
    }

    int64_t eval_int(const std::string& expr, const Record& env = Record{}) {
        Value out;
        std::string err;
        EXPECT_TRUE(eval(expr, env, &out, &err)) << expr << "\n" << err;
        EXPECT_EQ(out.type, INT_64) << expr;
        return out.int64;
    }

    std::string eval_str(const std::string& expr, const Record& env = Record{}) {
        Value out;
        std::string err;
        EXPECT_TRUE(eval(expr, env, &out, &err)) << expr << "\n" << err;
        return out.str;
    }

    std::string eval_error(const std::string& expr, const Record& env = Record{}) {
        Value out;
        std::string err = "<evaluated, but shouldn't have>";
        EXPECT_FALSE(eval(expr, env, &out, &err)) << expr;
        return err;
    }

    Record user_row(int64_t id, const std::string& name, int64_t age) {
        Record rec;
        rec.add_int64("id", id).add_str("name", name).add_int64("age", age);
        return rec;
    }

    class QLExecTest : public ::testing::Test {
    protected:
        void SetUp() override {
            const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
            path_ = (std::filesystem::temp_directory_path() /
                     (std::string("ql_exec_test_") + info->test_suite_name() + "_" + info->name() + ".db"))
                        .string();
            std::filesystem::remove(path_);
            reopen();
        }

        void TearDown() override {
            db_.reset();
            kv_.reset();
            std::filesystem::remove(path_);
        }

        void reopen() {
            db_.reset();
            kv_.reset();
            kv_ = std::make_unique<KV>(path_);
            kv_->open();
            db_ = std::make_unique<DB>(kv_.get());
        }

        // Runs sql in its own transaction, committed on success and rolled back on failure.
        QLResult run(const std::string& sql) {
            DBTX tx;
            db_->begin(&tx);
            QLResult res;
            std::string err;
            bool ok = ql_run(sql, &tx, &res, &err);
            EXPECT_TRUE(ok) << sql << "\n" << err;
            if (ok) {
                db_->commit(&tx);
            } else {
                db_->abort(&tx);
            }
            return res;
        }

        // The message from a statement that must fail.
        std::string run_error(const std::string& sql) {
            DBTX tx;
            db_->begin(&tx);
            QLResult res;
            std::string err = "<ran, but shouldn't have>";
            EXPECT_FALSE(ql_run(sql, &tx, &res, &err)) << sql;
            db_->abort(&tx);
            return err;
        }

        Rows select(const std::string& sql) {
            return to_text(run(sql).rows);
        }

        // The same statement through a read-only snapshot.
        Rows select_as_reader(const std::string& sql) {
            DBReader tx;
            db_->begin_read(&tx);
            QLResult res;
            std::string err;
            EXPECT_TRUE(ql_run(sql, &tx, &res, &err)) << sql << "\n" << err;
            db_->end_read(&tx);
            return to_text(res.rows);
        }

        void create_users() {
            run("CREATE TABLE users (id INT64, name STR, age INT64, PRIMARY KEY (id))");
        }

        // The same table with age indexed. CREATE TABLE has no INDEX clause, so it goes through the catalog API.
        void create_indexed_users() {
            TableDef users = TableDefBuilder("users")
                .add_col("id", INT_64)
                .add_col("name", BYTES)
                .add_col("age", INT_64)
                .set_pkeys(1)
                .add_index({"age"})
                .build();
            DBTX tx;
            db_->begin(&tx);
            std::string err;
            ASSERT_TRUE(tx.table_new(users, &err)) << err;
            db_->commit(&tx);
        }

        void insert_users() {
            run("INSERT INTO users (id, name, age) VALUES (1, 'ann', 30), (2, 'bob', 20), (3, 'cid', 40)");
        }

        std::string path_;
        std::unique_ptr<KV> kv_;
        std::unique_ptr<DB> db_;
    };
}

// ============================================================================
// Expression evaluation
// ============================================================================
TEST(QLEvalTest, WhenArithmeticIsEvaluatedThenPrecedenceHolds) {
    EXPECT_EQ(eval_int("1 + 2 * 3"), 7);
    EXPECT_EQ(eval_int("(1 + 2) * 3"), 9);
    EXPECT_EQ(eval_int("7 / 2"), 3);
    EXPECT_EQ(eval_int("7 % 2"), 1);
    EXPECT_EQ(eval_int("-5"), -5);
    EXPECT_EQ(eval_int("1 - -2"), 3);
}

TEST(QLEvalTest, WhenALiteralIsEvaluatedThenItIsItsOwnValue) {
    EXPECT_EQ(eval_int("42"), 42);
    EXPECT_EQ(eval_str("'hi'"), "hi");
}

TEST(QLEvalTest, WhenAColumnIsReferencedThenTheRowSuppliesIt) {
    Record env = user_row(1, "ann", 30);

    EXPECT_EQ(eval_int("age + 2", env), 32);
    EXPECT_EQ(eval_str("name", env), "ann");
}

TEST(QLEvalTest, WhenAColumnIsUnknownThenEvalFails) {
    EXPECT_NE(eval_error("nope", user_row(1, "ann", 30)).find("unknown column: nope"), std::string::npos);
}

TEST(QLEvalTest, WhenComparisonsAreEvaluatedThenTheyYieldZeroOrOne) {
    EXPECT_EQ(eval_int("1 < 2"), 1);
    EXPECT_EQ(eval_int("2 < 1"), 0);
    EXPECT_EQ(eval_int("2 >= 2"), 1);
    EXPECT_EQ(eval_int("1 = 1"), 1);
    EXPECT_EQ(eval_int("1 != 1"), 0);
}

TEST(QLEvalTest, WhenStringsAreComparedThenTheyOrderLexicographically) {
    EXPECT_EQ(eval_int("'ann' < 'bob'"), 1);
    EXPECT_EQ(eval_int("'ann' = 'ann'"), 1);
    EXPECT_EQ(eval_int("'b' < 'ab'"), 0);
}

TEST(QLEvalTest, WhenBooleanOperatorsCombineThenTheyFollowTheTruthTables) {
    EXPECT_EQ(eval_int("1 AND 1"), 1);
    EXPECT_EQ(eval_int("1 AND 0"), 0);
    EXPECT_EQ(eval_int("0 OR 1"), 1);
    EXPECT_EQ(eval_int("0 OR 0"), 0);
    EXPECT_EQ(eval_int("NOT 0"), 1);
    EXPECT_EQ(eval_int("NOT 5"), 0);
    EXPECT_EQ(eval_int("1 = 1 AND 2 > 3"), 0);
}

TEST(QLEvalTest, WhenAComparisonMixesTypesThenEvalFails) {
    EXPECT_NE(eval_error("1 = 'a'").find("cannot compare"), std::string::npos);
}

TEST(QLEvalTest, WhenArithmeticGetsAStringThenEvalFails) {
    EXPECT_NE(eval_error("'a' + 1").find("arithmetic needs numbers"), std::string::npos);
}

TEST(QLEvalTest, WhenABooleanOperatorGetsAStringThenEvalFails) {
    EXPECT_NE(eval_error("'a' AND 1").find("AND needs booleans"), std::string::npos);
    EXPECT_NE(eval_error("NOT 'a'").find("NOT needs a boolean"), std::string::npos);
}

TEST(QLEvalTest, WhenDividingByZeroThenEvalFails) {
    EXPECT_NE(eval_error("1 / 0").find("division by zero"), std::string::npos);
    EXPECT_NE(eval_error("1 % 0").find("modulo by zero"), std::string::npos);
}

TEST(QLEvalTest, WhenArithmeticOverflowsThenEvalFailsInsteadOfWrapping) {
    EXPECT_NE(eval_error("9223372036854775807 + 1").find("integer overflow"), std::string::npos);
    EXPECT_NE(eval_error("9223372036854775807 * 2").find("integer overflow"), std::string::npos);
    EXPECT_EQ(eval_int("9223372036854775807"), I64_MAX);
}

TEST(QLEvalTest, WhenATupleIsEvaluatedThenEvalFails) {
    EXPECT_NE(eval_error("(1, 2)").find("tuple"), std::string::npos);
}

// ============================================================================
// Statements
// ============================================================================
TEST_F(QLExecTest, WhenATableIsCreatedThenItAcceptsRows) {
    create_users();

    EXPECT_EQ(run("INSERT INTO users (id, name, age) VALUES (1, 'ann', 30)").count, 1u);
    EXPECT_EQ(select("SELECT id, name, age FROM users"), (Rows{"id=1 name=ann age=30"}));
}

TEST_F(QLExecTest, WhenATableIsCreatedTwiceThenTheSecondFails) {
    create_users();

    EXPECT_NE(run_error("CREATE TABLE users (id INT64, PRIMARY KEY (id))").find("users"), std::string::npos);
}

TEST_F(QLExecTest, WhenSelectIsStarThenEveryColumnComesBackInSchemaOrder) {
    create_users();
    insert_users();

    EXPECT_EQ(select("SELECT * FROM users WHERE id = 2"), (Rows{"id=2 name=bob age=20"}));
}

TEST_F(QLExecTest, WhenSelectHasExpressionsThenTheyAreEvaluatedPerRow) {
    create_users();
    insert_users();

    EXPECT_EQ(select("SELECT id, age + 1 AS next FROM users"),
              (Rows{"id=1 next=31", "id=2 next=21", "id=3 next=41"}));
}

TEST_F(QLExecTest, WhenRowsAreReturnedThenTheyComeInPrimaryKeyOrder) {
    create_users();
    run("INSERT INTO users (id, name, age) VALUES (3, 'cid', 40), (1, 'ann', 30), (2, 'bob', 20)");

    EXPECT_EQ(select("SELECT id FROM users"), (Rows{"id=1", "id=2", "id=3"}));
}

TEST_F(QLExecTest, WhenWhereFiltersRowsThenOnlyMatchesComeBack) {
    create_users();
    insert_users();

    EXPECT_EQ(select("SELECT id FROM users WHERE age >= 30"), (Rows{"id=1", "id=3"}));
    EXPECT_EQ(select("SELECT id FROM users WHERE name = 'bob'"), (Rows{"id=2"}));
    EXPECT_EQ(select("SELECT id FROM users WHERE age > 100"), (Rows{}));
}

TEST_F(QLExecTest, WhenWhereCombinesConditionsThenAllOfThemApply) {
    create_users();
    insert_users();

    EXPECT_EQ(select("SELECT id FROM users WHERE age > 25 AND name != 'cid'"), (Rows{"id=1"}));
    EXPECT_EQ(select("SELECT id FROM users WHERE age < 25 OR age > 35"), (Rows{"id=2", "id=3"}));
    EXPECT_EQ(select("SELECT id FROM users WHERE NOT age = 30"), (Rows{"id=2", "id=3"}));
}

TEST_F(QLExecTest, WhenTheFilterSpellingIsUsedThenItFiltersToo) {
    create_users();
    insert_users();

    EXPECT_EQ(select("SELECT id FROM users FILTER age >= 30"), (Rows{"id=1", "id=3"}));
}

TEST_F(QLExecTest, WhenWhereIsNotABooleanThenTheStatementFails) {
    create_users();
    insert_users();

    EXPECT_NE(run_error("SELECT id FROM users WHERE name").find("WHERE is not a boolean"), std::string::npos);
}

TEST_F(QLExecTest, WhenLimitIsGivenThenItCountsMatchingRows) {
    create_users();
    insert_users();

    EXPECT_EQ(select("SELECT id FROM users LIMIT 2"), (Rows{"id=1", "id=2"}));
    EXPECT_EQ(select("SELECT id FROM users WHERE age >= 30 LIMIT 1"), (Rows{"id=1"}));
    EXPECT_EQ(select("SELECT id FROM users LIMIT 0"), (Rows{}));
}

TEST_F(QLExecTest, WhenLimitHasAnOffsetThenItSkipsMatchingRows) {
    create_users();
    insert_users();

    EXPECT_EQ(select("SELECT id FROM users LIMIT 1, 2"), (Rows{"id=2", "id=3"}));
    EXPECT_EQ(select("SELECT id FROM users WHERE age >= 30 LIMIT 1, 5"), (Rows{"id=3"}));
    EXPECT_EQ(select("SELECT id FROM users LIMIT 9, 5"), (Rows{}));
}

TEST_F(QLExecTest, WhenInsertHasSeveralRowsThenAllOfThemLand) {
    create_users();

    EXPECT_EQ(run("INSERT INTO users (id, name, age) VALUES (1, 'ann', 30), (2, 'bob', 20)").count, 2u);
    EXPECT_EQ(select("SELECT id FROM users"), (Rows{"id=1", "id=2"}));
}

TEST_F(QLExecTest, WhenInsertValuesAreExpressionsThenTheyAreEvaluated) {
    create_users();
    run("INSERT INTO users (id, name, age) VALUES (1 + 1, 'a', 10 * 3)");

    EXPECT_EQ(select("SELECT id, age FROM users"), (Rows{"id=2 age=30"}));
}

TEST_F(QLExecTest, WhenAnInsertValueNamesAColumnThenItFails) {
    create_users();

    EXPECT_NE(run_error("INSERT INTO users (id, name, age) VALUES (1, 'a', age)").find("unknown column: age"),
              std::string::npos);
}

TEST_F(QLExecTest, WhenInsertRepeatsAPrimaryKeyThenItFails) {
    create_users();
    insert_users();

    EXPECT_NE(run_error("INSERT INTO users (id, name, age) VALUES (1, 'dup', 1)").find("already exists"),
              std::string::npos);
}

TEST_F(QLExecTest, WhenInsertOmitsAColumnThenItFails) {
    create_users();

    EXPECT_NE(run_error("INSERT INTO users (id, name) VALUES (1, 'ann')").find("age"), std::string::npos);
}

TEST_F(QLExecTest, WhenAnInsertRowHasTheWrongArityThenItFails) {
    create_users();

    EXPECT_NE(run_error("INSERT INTO users (id, name, age) VALUES (1, 'ann')").find("2 values"), std::string::npos);
}

TEST_F(QLExecTest, WhenAnInsertValueHasTheWrongTypeThenItFails) {
    create_users();

    EXPECT_NE(run_error("INSERT INTO users (id, name, age) VALUES ('one', 'ann', 30)").find("id"), std::string::npos);
}

TEST_F(QLExecTest, WhenUpdateSetsColumnsThenTheMatchingRowsChange) {
    create_users();
    insert_users();

    EXPECT_EQ(run("UPDATE users SET age = age + 1 WHERE id = 1").count, 1u);
    EXPECT_EQ(select("SELECT id, age FROM users"), (Rows{"id=1 age=31", "id=2 age=20", "id=3 age=40"}));
}

TEST_F(QLExecTest, WhenUpdateHasNoWhereThenEveryRowChanges) {
    create_users();
    insert_users();

    EXPECT_EQ(run("UPDATE users SET name = 'x'").count, 3u);
    EXPECT_EQ(select("SELECT name FROM users"), (Rows{"name=x", "name=x", "name=x"}));
}

TEST_F(QLExecTest, WhenUpdateAssignsFromOtherColumnsThenItSeesTheOldRow) {
    run("CREATE TABLE pairs (id INT64, a INT64, b INT64, PRIMARY KEY (id))");
    run("INSERT INTO pairs (id, a, b) VALUES (1, 10, 20)");

    run("UPDATE pairs SET a = b, b = a");

    EXPECT_EQ(select("SELECT a, b FROM pairs"), (Rows{"a=20 b=10"}));
}

TEST_F(QLExecTest, WhenUpdateTargetsThePrimaryKeyThenItFails) {
    create_users();
    insert_users();

    EXPECT_NE(run_error("UPDATE users SET id = 9 WHERE id = 1").find("cannot update a primary key column"),
              std::string::npos);
}

TEST_F(QLExecTest, WhenUpdateTargetsAnUnknownColumnThenItFails) {
    create_users();

    EXPECT_NE(run_error("UPDATE users SET nope = 1").find("unknown column: nope"), std::string::npos);
}

TEST_F(QLExecTest, WhenUpdateIsLimitedThenOnlyThatManyRowsChange) {
    create_users();
    insert_users();

    EXPECT_EQ(run("UPDATE users SET name = 'x' LIMIT 1").count, 1u);
    EXPECT_EQ(select("SELECT name FROM users"), (Rows{"name=x", "name=bob", "name=cid"}));
}

TEST_F(QLExecTest, WhenDeleteHasAWhereThenOnlyMatchingRowsGo) {
    create_users();
    insert_users();

    EXPECT_EQ(run("DELETE FROM users WHERE age < 30").count, 1u);
    EXPECT_EQ(select("SELECT id FROM users"), (Rows{"id=1", "id=3"}));
}

TEST_F(QLExecTest, WhenDeleteHasNoWhereThenEveryRowGoes) {
    create_users();
    insert_users();

    EXPECT_EQ(run("DELETE FROM users").count, 3u);
    EXPECT_EQ(select("SELECT id FROM users"), (Rows{}));
}

TEST_F(QLExecTest, WhenDeleteMatchesNothingThenItCountsZero) {
    create_users();
    insert_users();

    EXPECT_EQ(run("DELETE FROM users WHERE age > 100").count, 0u);
    EXPECT_EQ(select("SELECT id FROM users").size(), 3u);
}

TEST_F(QLExecTest, WhenTheTableIsUnknownThenStatementsFail) {
    EXPECT_NE(run_error("SELECT a FROM nope").find("table not found"), std::string::npos);
    EXPECT_NE(run_error("INSERT INTO nope (a) VALUES (1)").find("table not found"), std::string::npos);
    EXPECT_NE(run_error("UPDATE nope SET a = 1").find("table not found"), std::string::npos);
    EXPECT_NE(run_error("DELETE FROM nope").find("table not found"), std::string::npos);
}

TEST_F(QLExecTest, WhenTheStatementDoesNotParseThenNothingRuns) {
    create_users();

    EXPECT_NE(run_error("SELECT FROM users").find("syntax error"), std::string::npos);
}

TEST_F(QLExecTest, WhenIndexByIsSetOnAScanThenExecutionRefusesIt) {
    create_users();
    insert_users();

    QLSelect req;
    req.scan.table = "users";
    req.scan.key1 = ql_binop(QL_CMP_GT, ql_sym("id"), ql_int64(1)); // the grammar cannot build this
    req.names.push_back("id");
    req.output.push_back(ql_sym("id"));

    DBTX tx;
    db_->begin(&tx);
    std::vector<Record> rows;
    std::string err;
    EXPECT_FALSE(ql_select(req, &tx, &rows, &err));
    db_->abort(&tx);

    EXPECT_NE(err.find("INDEX BY is not supported"), std::string::npos) << err;
}

// ============================================================================
// Index selection (see ql/ql_range.h)
// ============================================================================
TEST_F(QLExecTest, WhenAnIndexedColumnIsFilteredThenTheRowsComeBackInIndexOrder) {
    create_indexed_users();
    insert_users();

    // The (age, id) index is scanned, so rows arrive by age rather than by id.
    EXPECT_EQ(select("SELECT id, age FROM users WHERE age >= 20"),
              (Rows{"id=2 age=20", "id=1 age=30", "id=3 age=40"}));
}

TEST_F(QLExecTest, WhenAnIndexedColumnIsFixedThenOnlyThoseRowsComeBack) {
    create_indexed_users();
    insert_users();

    EXPECT_EQ(select("SELECT id FROM users WHERE age = 30"), (Rows{"id=1"}));
    EXPECT_EQ(select("SELECT id FROM users WHERE age = 99"), (Rows{}));
}

TEST_F(QLExecTest, WhenTheRangeIsWiderThanTheConditionThenTheFilterStillExcludesRows) {
    create_indexed_users();
    insert_users();

    // age >= 20 picks the index range; name != 'bob' is left to the filter.
    EXPECT_EQ(select("SELECT id FROM users WHERE age >= 20 AND name != 'bob'"), (Rows{"id=1", "id=3"}));
}

TEST_F(QLExecTest, WhenTheFilterUsesOrThenEveryRowIsStillChecked) {
    create_indexed_users();
    insert_users();

    // OR narrows nothing, so this is a full scan, and it must still return the right rows.
    EXPECT_EQ(select("SELECT id FROM users WHERE age = 20 OR age = 40"), (Rows{"id=2", "id=3"}));
}

TEST_F(QLExecTest, WhenAnUpdateMovesRowsInsideTheIndexItScannedThenEachRowChangesOnce) {
    create_indexed_users();
    insert_users();

    // Rows are collected before the first write, so shifting them within the (age, id) index cannot make the scan
    // revisit a row or skip one.
    EXPECT_EQ(run("UPDATE users SET age = age + 5 WHERE age >= 20").count, 3u);
    EXPECT_EQ(select("SELECT id, age FROM users"), (Rows{"id=1 age=35", "id=2 age=25", "id=3 age=45"}));
}

TEST_F(QLExecTest, WhenADeleteFiltersOnAnIndexThenOnlyMatchingRowsGo) {
    create_indexed_users();
    insert_users();

    EXPECT_EQ(run("DELETE FROM users WHERE age = 20").count, 1u);
    EXPECT_EQ(select("SELECT id FROM users"), (Rows{"id=1", "id=3"}));
}

// ============================================================================
// End to end, through the whole database
// ============================================================================
TEST_F(QLExecTest, WhenRowsAreWrittenThroughSqlThenSecondaryIndexesStayInSync) {
    create_indexed_users();
    insert_users();
    run("UPDATE users SET age = 25 WHERE id = 1");
    run("DELETE FROM users WHERE id = 3");

    // Walking the (age, id) index must show exactly the rows SQL left behind.
    DBReader tx;
    db_->begin_read(&tx);
    Scanner sc(CMP_GE, CMP_LE, age_key(I64_MIN), age_key(I64_MAX));
    std::string err;
    ASSERT_TRUE(tx.scan("users", &sc, &err)) << err;
    std::vector<std::string> by_age;
    for (; sc.valid(); sc.next()) {
        Record rec;
        sc.deref(&rec); // asserts if an index key has no row
        by_age.push_back(to_text(rec));
    }
    db_->end_read(&tx);

    EXPECT_EQ(by_age, (Rows{"id=2 name=bob age=20", "id=1 name=ann age=25"}));
}

TEST_F(QLExecTest, WhenAStatementFailsMidwayThenTheTransactionRollsBack) {
    create_users();
    insert_users();

    // The second row repeats a primary key, so the whole statement is rolled back.
    DBTX tx;
    db_->begin(&tx);
    QLResult res;
    std::string err;
    EXPECT_FALSE(ql_run("INSERT INTO users (id, name, age) VALUES (4, 'dee', 50), (1, 'dup', 1)", &tx, &res, &err));
    db_->abort(&tx);

    EXPECT_EQ(select("SELECT id FROM users"), (Rows{"id=1", "id=2", "id=3"}));
}

TEST_F(QLExecTest, WhenSeveralStatementsShareATransactionThenTheyCommitTogether) {
    create_users();

    DBTX tx;
    db_->begin(&tx);
    QLResult res;
    std::string err;
    ASSERT_TRUE(ql_run("INSERT INTO users (id, name, age) VALUES (1, 'ann', 30)", &tx, &res, &err)) << err;
    ASSERT_TRUE(ql_run("INSERT INTO users (id, name, age) VALUES (2, 'bob', 20)", &tx, &res, &err)) << err;
    ASSERT_TRUE(ql_run("UPDATE users SET age = 99 WHERE id = 1", &tx, &res, &err)) << err;
    // The transaction sees its own writes before the commit.
    ASSERT_TRUE(ql_run("SELECT id, age FROM users", &tx, &res, &err)) << err;
    EXPECT_EQ(to_text(res.rows), (Rows{"id=1 age=99", "id=2 age=20"}));
    db_->commit(&tx);

    EXPECT_EQ(select("SELECT id, age FROM users"), (Rows{"id=1 age=99", "id=2 age=20"}));
}

TEST_F(QLExecTest, WhenTheDatabaseIsReopenedThenSqlWrittenRowsAreStillThere) {
    create_users();
    insert_users();

    reopen();

    EXPECT_EQ(select("SELECT id, name FROM users"), (Rows{"id=1 name=ann", "id=2 name=bob", "id=3 name=cid"}));
}

TEST_F(QLExecTest, WhenAReaderIsOpenThenItKeepsSeeingItsOwnSnapshot) {
    create_users();
    insert_users();

    DBReader reader;
    db_->begin_read(&reader);

    run("INSERT INTO users (id, name, age) VALUES (4, 'dee', 50)");

    QLResult res;
    std::string err;
    ASSERT_TRUE(ql_run("SELECT id FROM users", &reader, &res, &err)) << err;
    db_->end_read(&reader);

    EXPECT_EQ(to_text(res.rows), (Rows{"id=1", "id=2", "id=3"})); // the row committed after it began is invisible
    EXPECT_EQ(select("SELECT id FROM users").size(), 4u);
}

TEST_F(QLExecTest, WhenSelectRunsOnAReaderThenItSeesCommittedRows) {
    create_users();
    insert_users();

    EXPECT_EQ(select_as_reader("SELECT id, name FROM users WHERE age >= 30"),
              (Rows{"id=1 name=ann", "id=3 name=cid"}));
}

TEST_F(QLExecTest, WhenAWriteStatementRunsOnAReaderThenItFails) {
    create_users();

    DBReader tx;
    db_->begin_read(&tx);
    QLResult res;
    std::string err;
    EXPECT_FALSE(ql_run("DELETE FROM users", &tx, &res, &err));
    db_->end_read(&tx);

    EXPECT_NE(err.find("needs a write transaction"), std::string::npos) << err;
}
