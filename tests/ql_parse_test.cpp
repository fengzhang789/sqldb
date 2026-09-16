#include "ql/ql_parse.h"

#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace {
    // Parses sql, or fails the test with the parser's own message.
    QLStatement parse(const std::string& sql) {
        QLStatement stmt;
        std::string err;
        EXPECT_TRUE(parse_statement(sql, &stmt, &err)) << sql << "\n" << err;
        return stmt;
    }

    // The message from a statement that must not parse.
    std::string parse_error(const std::string& sql) {
        QLStatement stmt;
        std::string err = "<parsed, but shouldn't have>";
        EXPECT_FALSE(parse_statement(sql, &stmt, &err)) << sql;
        return err;
    }

    std::string select_expr(const std::string& expr) {
        return to_string(std::get<QLSelect>(parse("SELECT " + expr + " FROM t")).output[0]);
    }

    std::string where_expr(const std::string& cond) {
        return to_string(std::get<QLSelect>(parse("SELECT a FROM t WHERE " + cond)).scan.filter);
    }
}

// ============================================================================
// Statement shapes
// ============================================================================
TEST(ParseCreateTableTest, WhenPrimaryKeyIsNamedLastThenItsColumnsMoveToTheFront) {
    TableDef def = std::get<QLCreateTable>(
        parse("CREATE TABLE users (name STR, id INT64, age INT64, PRIMARY KEY (id));")).def;

    EXPECT_EQ(def.name, "users");
    EXPECT_EQ(def.pkeys, 1);
    EXPECT_EQ(def.cols, (std::vector<std::string>{"id", "name", "age"}));
    EXPECT_EQ(def.types, (std::vector<ValueType>{INT_64, BYTES, INT_64}));
}

TEST(ParseCreateTableTest, WhenThePrimaryKeyIsCompositeThenItKeepsTheOrderTheClauseNamed) {
    TableDef def = std::get<QLCreateTable>(
        parse("CREATE TABLE t (a INT, b INT, c INT, PRIMARY KEY (c, a));")).def;

    EXPECT_EQ(def.pkeys, 2);
    EXPECT_EQ(def.cols, (std::vector<std::string>{"c", "a", "b"}));
}

TEST(ParseCreateTableTest, WhenTypeNamesVaryThenTheyMapOntoValueTypes) {
    TableDef def = std::get<QLCreateTable>(
        parse("CREATE TABLE t (a int, b bigint, c text, d varchar, PRIMARY KEY (a));")).def;

    EXPECT_EQ(def.types, (std::vector<ValueType>{INT_64, INT_64, BYTES, BYTES}));
}

TEST(ParseCreateTableTest, WhenAColumnTypeIsUnknownThenParseFails) {
    EXPECT_NE(parse_error("CREATE TABLE t (a float, PRIMARY KEY (a));").find("unknown column type: float"),
              std::string::npos);
}

TEST(ParseCreateTableTest, WhenAPrimaryKeyColumnIsUndeclaredThenParseFails) {
    EXPECT_NE(parse_error("CREATE TABLE t (a INT, PRIMARY KEY (b));").find("primary key column is not defined: b"),
              std::string::npos);
}

TEST(ParseSelectTest, WhenEveryClauseIsPresentThenAllOfThemAppearInTheTree) {
    QLSelect sel = std::get<QLSelect>(parse("SELECT id, age + 1 AS next FROM users WHERE age >= 18 LIMIT 10, 5;"));

    EXPECT_EQ(sel.scan.table, "users");
    EXPECT_EQ(sel.names, (std::vector<std::string>{"id", "next"}));
    EXPECT_EQ(to_string(sel.output[0]), "id");
    EXPECT_EQ(to_string(sel.output[1]), "(+ age 1)");
    EXPECT_EQ(to_string(sel.scan.filter), "(>= age 18)");
    EXPECT_EQ(sel.scan.offset, 10);
    EXPECT_EQ(sel.scan.limit, 5);
}

TEST(ParseSelectTest, WhenAnItemHasNoAliasThenItIsNamedAfterItsColumnOrPosition) {
    QLSelect sel = std::get<QLSelect>(parse("SELECT id, 1 + 1, name AS who FROM t"));

    EXPECT_EQ(sel.names, (std::vector<std::string>{"id", "col2", "who"}));
}

TEST(ParseSelectTest, WhenTheListIsStarThenTheOutputIsASingleStarNode) {
    QLSelect sel = std::get<QLSelect>(parse("SELECT * FROM t"));

    EXPECT_EQ(sel.names, (std::vector<std::string>{"*"}));
    ASSERT_EQ(sel.output.size(), 1u);
    EXPECT_EQ(sel.output[0].type, QL_STAR);
}

TEST(ParseSelectTest, WhenLimitHasOneNumberThenItIsTheCountAndOffsetIsZero) {
    QLSelect sel = std::get<QLSelect>(parse("SELECT a FROM t LIMIT 7"));

    EXPECT_EQ(sel.scan.offset, 0);
    EXPECT_EQ(sel.scan.limit, 7);
}

TEST(ParseSelectTest, WhenThereIsNoLimitThenTheScanIsUnlimited) {
    EXPECT_EQ(std::get<QLSelect>(parse("SELECT a FROM t")).scan.limit, -1);
}

TEST(ParseInsertTest, WhenSeveralRowsAreGivenThenEachBecomesItsOwnValueList) {
    QLInsert ins = std::get<QLInsert>(parse("INSERT INTO users (id, name) VALUES (1, 'ann'), (2, 'bob');"));

    EXPECT_EQ(ins.table, "users");
    EXPECT_EQ(ins.mode, UpdateMode::INSERT_ONLY);
    EXPECT_EQ(ins.names, (std::vector<std::string>{"id", "name"}));
    ASSERT_EQ(ins.values.size(), 2u);
    ASSERT_EQ(ins.values[0].size(), 2u);
    EXPECT_EQ(to_string(ins.values[0][0]), "1");
    EXPECT_EQ(to_string(ins.values[0][1]), "'ann'");
    EXPECT_EQ(to_string(ins.values[1][1]), "'bob'");
}

TEST(ParseInsertTest, WhenAValueIsAnExpressionThenItIsKeptUnevaluated) {
    QLInsert ins = std::get<QLInsert>(parse("INSERT INTO t (a, b) VALUES (1 + 2, -3)"));

    EXPECT_EQ(to_string(ins.values[0][0]), "(+ 1 2)");
    EXPECT_EQ(to_string(ins.values[0][1]), "(neg 3)");
}

TEST(ParseUpdateTest, WhenColumnsAreAssignedThenNamesAndValuesAreParallel) {
    QLUpdate upd = std::get<QLUpdate>(parse("UPDATE users SET age = age + 1, city = 'nyc' WHERE id = 3 LIMIT 1;"));

    EXPECT_EQ(upd.scan.table, "users");
    EXPECT_EQ(upd.names, (std::vector<std::string>{"age", "city"}));
    EXPECT_EQ(to_string(upd.values[0]), "(+ age 1)");
    EXPECT_EQ(to_string(upd.values[1]), "'nyc'");
    EXPECT_EQ(to_string(upd.scan.filter), "(= id 3)");
    EXPECT_EQ(upd.scan.limit, 1);
}

TEST(ParseDeleteTest, WhenAWhereIsGivenThenItBecomesTheScanFilter) {
    QLDelete del = std::get<QLDelete>(parse("DELETE FROM users WHERE age < 18;"));

    EXPECT_EQ(del.scan.table, "users");
    EXPECT_EQ(to_string(del.scan.filter), "(< age 18)");
}

TEST(ParseStatementTest, WhenAScanHasNoIndexByThenItsRangeBoundsStayUninitialized) {
    QLSelect sel = std::get<QLSelect>(parse("SELECT a FROM t WHERE a > 1"));

    EXPECT_EQ(sel.scan.key1.type, QL_UNINIT);
    EXPECT_EQ(sel.scan.key2.type, QL_UNINIT);
}

// ============================================================================
// Operator precedence
// ============================================================================
TEST(ParsePrecedenceTest, WhenMultiplicationMeetsAdditionThenMultiplicationBindsTighter) {
    EXPECT_EQ(select_expr("1 + 2 * 3"), "(+ 1 (* 2 3))");
    EXPECT_EQ(select_expr("1 * 2 + 3"), "(+ (* 1 2) 3)");
}

TEST(ParsePrecedenceTest, WhenAndMeetsOrThenAndBindsTighter) {
    EXPECT_EQ(where_expr("a AND b OR c"), "(or (and a b) c)");
    EXPECT_EQ(where_expr("a OR b AND c"), "(or a (and b c))");
}

TEST(ParsePrecedenceTest, WhenNotMeetsAComparisonThenTheComparisonBindsTighter) {
    EXPECT_EQ(where_expr("NOT a = b"), "(not (= a b))");
}

TEST(ParsePrecedenceTest, WhenNotMeetsAndThenNotBindsTighter) {
    EXPECT_EQ(where_expr("NOT a AND b"), "(and (not a) b)");
}

TEST(ParsePrecedenceTest, WhenParenthesesAreUsedThenTheyOverridePrecedence) {
    EXPECT_EQ(select_expr("(1 + 2) * 3"), "(* (+ 1 2) 3)");
    EXPECT_EQ(where_expr("NOT (a AND b)"), "(not (and a b))");
    EXPECT_EQ(where_expr("a AND (b OR c)"), "(and a (or b c))");
}

TEST(ParsePrecedenceTest, WhenMinusIsUnaryThenItBindsTighterThanMultiplication) {
    EXPECT_EQ(select_expr("-a * b"), "(* (neg a) b)");
    EXPECT_EQ(select_expr("1 - -2"), "(- 1 (neg 2))");
}

TEST(ParsePrecedenceTest, WhenTheSameOperatorRepeatsThenItGroupsToTheLeft) {
    EXPECT_EQ(select_expr("1 - 2 - 3"), "(- (- 1 2) 3)");
    EXPECT_EQ(select_expr("1 / 2 % 3"), "(% (/ 1 2) 3)");
}

TEST(ParsePrecedenceTest, WhenComparisonMeetsArithmeticThenArithmeticBindsTighter) {
    EXPECT_EQ(where_expr("a + 1 < b * 2"), "(< (+ a 1) (* b 2))");
}

TEST(ParsePrecedenceTest, WhenComparisonsAreChainedThenParseFails) {
    EXPECT_NE(parse_error("SELECT a FROM t WHERE a = b = c").find("syntax error"), std::string::npos);
}

// ============================================================================
// WHERE
// ============================================================================
TEST(ParseWhereTest, WhenConditionsCombineAndOrNotThenTheTreeFollowsPrecedence) {
    EXPECT_EQ(where_expr("a = 1 AND b != 2 OR NOT c <= 3"), "(or (and (= a 1) (!= b 2)) (not (<= c 3)))");
}

TEST(ParseWhereTest, WhenEveryComparisonIsUsedThenEachMapsToItsOwnNode) {
    EXPECT_EQ(where_expr("a >= 1"), "(>= a 1)");
    EXPECT_EQ(where_expr("a > 1"), "(> a 1)");
    EXPECT_EQ(where_expr("a < 1"), "(< a 1)");
    EXPECT_EQ(where_expr("a <= 1"), "(<= a 1)");
    EXPECT_EQ(where_expr("a = 1"), "(= a 1)");
    EXPECT_EQ(where_expr("a != 1"), "(!= a 1)");
    EXPECT_EQ(where_expr("a <> 1"), "(!= a 1)");
}

TEST(ParseWhereTest, WhenTheClauseIsSpelledFilterThenItParsesTheSameWay) {
    QLSelect where = std::get<QLSelect>(parse("SELECT a FROM t WHERE a > 1"));
    QLSelect filter = std::get<QLSelect>(parse("SELECT a FROM t FILTER a > 1"));

    EXPECT_EQ(filter.scan.filter, where.scan.filter);
}

TEST(ParseWhereTest, WhenDeleteAndUpdateAreFilteredThenTheyAcceptTheSameConditions) {
    EXPECT_EQ(to_string(std::get<QLDelete>(parse("DELETE FROM t WHERE a > 1 AND b < 2")).scan.filter),
              "(and (> a 1) (< b 2))");
    EXPECT_EQ(to_string(std::get<QLUpdate>(parse("UPDATE t SET a = 1 WHERE a > 1 AND b < 2")).scan.filter),
              "(and (> a 1) (< b 2))");
}

TEST(ParseWhereTest, WhenThereIsNoWhereThenTheFilterIsUninitialized) {
    EXPECT_EQ(std::get<QLSelect>(parse("SELECT a FROM t")).scan.filter.type, QL_UNINIT);
}

// INDEX BY is the book's clause, replaced here by WHERE, so `index` is left an ordinary identifier.
TEST(ParseWhereTest, WhenIndexByIsUsedThenParseFails) {
    EXPECT_NE(parse_error("SELECT a FROM t INDEX BY a > 1").find("syntax error"), std::string::npos);
}

TEST(ParseWhereTest, WhenIndexIsUsedAsAColumnNameThenItIsAnOrdinaryIdentifier) {
    EXPECT_EQ(where_expr("index > 1"), "(> index 1)");
}

// ============================================================================
// Tuples
// ============================================================================
TEST(ParseTupleTest, WhenParenthesesHoldSeveralExpressionsThenTheyBecomeATuple) {
    EXPECT_EQ(select_expr("(a, b, c)"), "(tuple a b c)");
}

TEST(ParseTupleTest, WhenParenthesesHoldOneExpressionThenThereIsNoTuple) {
    EXPECT_EQ(select_expr("(a)"), "a");
}

TEST(ParseTupleTest, WhenTuplesAreComparedThenBothSidesAreTuples) {
    EXPECT_EQ(where_expr("(a, b) < (1, 2)"), "(< (tuple a b) (tuple 1 2))");
}

TEST(ParseTupleTest, WhenATupleHoldsExpressionsThenEachKidKeepsItsOwnTree) {
    EXPECT_EQ(select_expr("(a + 1, -b)"), "(tuple (+ a 1) (neg b))");
}

// ============================================================================
// Literals, keywords and comments
// ============================================================================
TEST(ParseLiteralTest, WhenAStringHasEscapesThenTheyAreDecoded) {
    QLSelect sel = std::get<QLSelect>(parse("SELECT 'a\\nb', 'it''s', 'q\\'d' FROM t"));

    EXPECT_EQ(sel.output[0].val.str, "a\nb");
    EXPECT_EQ(sel.output[1].val.str, "it's");
    EXPECT_EQ(sel.output[2].val.str, "q'd");
}

TEST(ParseLiteralTest, WhenKeywordsAreMixedCaseThenTheyStillMatch) {
    QLSelect sel = std::get<QLSelect>(parse("sElEcT a FrOm t WhErE a AnD b"));

    EXPECT_EQ(sel.scan.table, "t");
    EXPECT_EQ(to_string(sel.scan.filter), "(and a b)");
}

TEST(ParseLiteralTest, WhenIdentifiersAreMixedCaseThenTheirCaseIsKept) {
    EXPECT_EQ(select_expr("MyCol"), "MyCol");
}

TEST(ParseLiteralTest, WhenCommentsAppearThenTheyAreSkipped) {
    QLSelect sel = std::get<QLSelect>(parse("SELECT a -- the column\nFROM /* the table */ t"));

    EXPECT_EQ(sel.scan.table, "t");
    EXPECT_EQ(sel.names, (std::vector<std::string>{"a"}));
}

TEST(ParseLiteralTest, WhenASemicolonIsOmittedThenTheStatementStillParses) {
    EXPECT_EQ(std::get<QLSelect>(parse("SELECT a FROM t")).scan.table, "t");
}

// ============================================================================
// Errors
// ============================================================================
TEST(ParseErrorTest, WhenAParenIsUnclosedThenParseFailsWithALocation) {
    std::string err = parse_error("SELECT (1 + 2 FROM t");

    EXPECT_TRUE(err.starts_with("1:")) << err;
    EXPECT_NE(err.find("syntax error"), std::string::npos) << err;
}

TEST(ParseErrorTest, WhenFromIsMissingThenParseFails) {
    std::string err = parse_error("SELECT a t");

    EXPECT_TRUE(err.starts_with("1:")) << err;
    EXPECT_NE(err.find("FROM"), std::string::npos) << err; // bison lists what it expected
}

TEST(ParseErrorTest, WhenTheStatementKeywordIsUnknownThenParseFails) {
    std::string err = parse_error("UPSERT INTO t (a) VALUES (1)");

    EXPECT_TRUE(err.starts_with("1:1:")) << err;
    EXPECT_NE(err.find("syntax error"), std::string::npos) << err;
}

TEST(ParseErrorTest, WhenTheInputIsEmptyThenParseFails) {
    EXPECT_NE(parse_error("").find("syntax error"), std::string::npos);
}

TEST(ParseErrorTest, WhenAStringIsUnterminatedThenParseFails) {
    EXPECT_NE(parse_error("SELECT 'abc FROM t").find("unterminated string literal"), std::string::npos);
}

TEST(ParseErrorTest, WhenAnIntegerIsOutOfRangeThenParseFails) {
    EXPECT_NE(parse_error("SELECT 99999999999999999999 FROM t").find("out of range"), std::string::npos);
}

TEST(ParseErrorTest, WhenACharacterIsUnexpectedThenParseFails) {
    EXPECT_NE(parse_error("SELECT a $ b FROM t").find("unexpected character: $"), std::string::npos);
}

TEST(ParseErrorTest, WhenTheErrorIsOnALaterLineThenTheLocationSaysSo) {
    std::string err = parse_error("SELECT a\nFROM t\nWHERE (a > 1");

    EXPECT_TRUE(err.starts_with("3:")) << err;
}

TEST(ParseErrorTest, WhenTrailingInputFollowsAStatementThenParseFails) {
    EXPECT_NE(parse_error("SELECT a FROM t; SELECT b FROM u").find("syntax error"), std::string::npos);
}

TEST(ParseErrorTest, WhenParsingFailsThenTheOutputIsUntouched) {
    QLStatement stmt = parse("SELECT a FROM t");
    std::string err;

    EXPECT_FALSE(parse_statement("SELECT FROM", &stmt, &err));
    EXPECT_EQ(std::get<QLSelect>(stmt).scan.table, "t");
}
