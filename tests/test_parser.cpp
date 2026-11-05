#include "minisql/parser.h"

#include <limits>

#include "test_util.h"

using namespace minisql;

static std::string reprint(const std::string& sql) { return ast::printExpr(*parseExpression(sql)); }

static ast::Statement parseOne(const std::string& sql) {
  auto statements = parseScript(sql);
  CHECK_EQ(statements.size(), size_t{1});
  return std::move(statements.front());
}

TEST(precedence_shows_when_printed) {
  CHECK_EQ(reprint("1 + 2 * 3"), std::string("1 + 2 * 3"));
  CHECK_EQ(reprint("(1 + 2) * 3"), std::string("(1 + 2) * 3"));
  CHECK_EQ(reprint("a OR b AND c"), std::string("a OR b AND c"));
  CHECK_EQ(reprint("(a OR b) AND c"), std::string("(a OR b) AND c"));
  CHECK_EQ(reprint("NOT a = 1"), std::string("NOT a = 1"));
}

TEST(subtraction_is_left_associative) {
  CHECK_EQ(reprint("1 - 2 - 3"), std::string("1 - 2 - 3"));
  CHECK_EQ(reprint("1 - (2 - 3)"), std::string("1 - (2 - 3)"));
}

TEST(predicate_forms) {
  CHECK_EQ(reprint("a IS NOT NULL"), std::string("a IS NOT NULL"));
  CHECK_EQ(reprint("a NOT IN (1, 2)"), std::string("a NOT IN (1, 2)"));
  CHECK_EQ(reprint("a NOT BETWEEN 1 AND 2"), std::string("a NOT BETWEEN 1 AND 2"));
  CHECK_EQ(reprint("a NOT LIKE 'x%'"), std::string("a NOT LIKE 'x%'"));
}

TEST(between_and_does_not_swallow_the_outer_and) {
  CHECK_EQ(reprint("a BETWEEN 1 AND 2 AND b = 3"), std::string("a BETWEEN 1 AND 2 AND b = 3"));
  const auto expr = parseExpression("a BETWEEN 1 AND 2 AND b = 3");
  const auto* top = std::get_if<ast::Binary>(&expr->node);
  CHECK(top != nullptr);
  CHECK(top->op == ast::BinaryOp::And);
}

TEST(double_negation_does_not_print_a_comment) {
  std::string text = reprint("- (-a)");
  CHECK(!testutil::contains(text, "--"));
  CHECK_EQ(reprint(text), text);
}

TEST(smallest_integer_literal_is_accepted) {
  auto expr = parseExpression("-9223372036854775808");
  const auto* literal = std::get_if<ast::Literal>(&expr->node);
  CHECK(literal != nullptr);
  CHECK_EQ(literal->value.asInt(), std::numeric_limits<int64_t>::min());
  CHECK_ERROR(parseExpression("9223372036854775808"), Syntax);
}

TEST(deep_nesting_is_an_error_not_a_crash) {
  std::string deep = std::string(100000, '(') + "1" + std::string(100000, ')');
  CHECK_ERROR(parseExpression(deep), Syntax);
  std::string longChain = "1";
  for (int i = 0; i < 100000; ++i) longChain += " + 1";
  CHECK_ERROR(parseExpression(longChain), Syntax);
  std::string manyNots;
  for (int i = 0; i < 100000; ++i) manyNots += "NOT ";
  CHECK_ERROR(parseExpression(manyNots + "a"), Syntax);
}

TEST(moderate_nesting_is_fine) {
  std::string nested = std::string(50, '(') + "1" + std::string(50, ')');
  CHECK(parseExpression(nested) != nullptr);
}

TEST(create_table_details) {
  auto statement = parseOne("CREATE TABLE IF NOT EXISTS t (a INT PRIMARY KEY, b VARCHAR(20) NOT NULL DEFAULT 'x')");
  const auto& create = std::get<ast::CreateTable>(statement.node);
  CHECK(create.ifNotExists);
  CHECK_EQ(create.columns.size(), size_t{2});
  CHECK(create.columns[0].primaryKey);
  CHECK(create.columns[1].type == Type::Text);
  CHECK(create.columns[1].notNull);
  CHECK(create.columns[1].defaultExpr != nullptr);
}

TEST(select_clauses) {
  auto statement = parseOne(
      "SELECT a AS x, b y, t.*, * FROM t AS u JOIN v ON u.a = v.a, w WHERE a > 1 "
      "GROUP BY a, b HAVING COUNT(*) > 1 ORDER BY a DESC, b ASC, c LIMIT 5 OFFSET 2");
  const auto& select = std::get<ast::Select>(statement.node);
  CHECK_EQ(select.items.size(), size_t{4});
  CHECK_EQ(select.items[0].alias, std::string("x"));
  CHECK_EQ(select.items[1].alias, std::string("y"));
  CHECK_EQ(select.items[2].starTable, std::string("t"));
  CHECK(select.items[3].expr == nullptr);
  CHECK_EQ(select.from->alias, std::string("u"));
  CHECK_EQ(select.joins.size(), size_t{2});
  CHECK(select.joins[0].on != nullptr);
  CHECK(select.joins[1].on == nullptr);
  CHECK_EQ(select.groupBy.size(), size_t{2});
  CHECK(select.orderBy[0].descending);
  CHECK(!select.orderBy[1].descending);
  CHECK(select.limit != nullptr);
  CHECK(select.offset != nullptr);
}

TEST(script_splits_on_semicolons) {
  CHECK_EQ(parseScript("SELECT 1; SELECT 2;; SELECT 3").size(), size_t{3});
  CHECK_EQ(parseScript("  -- nothing\n").size(), size_t{0});
}

TEST(statements_need_separators) { CHECK_ERROR(parseScript("SELECT 1 SELECT 2"), Syntax); }

TEST(soft_keywords_work_as_names) {
  CHECK_EQ(parseScript("CREATE TABLE key (index INT, transaction TEXT)").size(), size_t{1});
  CHECK_EQ(parseScript("SELECT key FROM begin").size(), size_t{1});
}

TEST(reserved_words_need_quotes) {
  CHECK_ERROR(parseScript("CREATE TABLE select (a INT)"), Syntax);
  CHECK_EQ(parseScript("CREATE TABLE \"select\" (a INT)").size(), size_t{1});
}

TEST(unsupported_features_say_so_clearly) {
  CHECK_ERROR(parseScript("SELECT DISTINCT a FROM t"), Syntax);
  CHECK_ERROR(parseScript("SELECT a FROM t LEFT JOIN u ON 1 = 1"), Syntax);
  CHECK_ERROR(parseScript("SELECT (SELECT 1)"), Syntax);
}

TEST(explain_only_wraps_row_statements) {
  CHECK_EQ(parseScript("EXPLAIN SELECT 1").size(), size_t{1});
  CHECK_EQ(parseScript("EXPLAIN DELETE FROM t").size(), size_t{1});
  CHECK_ERROR(parseScript("EXPLAIN DROP TABLE t"), Syntax);
  CHECK_ERROR(parseScript("EXPLAIN EXPLAIN SELECT 1"), Syntax);
}

TEST(syntax_errors_have_positions) {
  try {
    parseScript("SELECT a,\n  FROM t");
    CHECK(false);
  } catch (const Error& e) {
    CHECK(e.kind() == ErrorKind::Syntax);
    CHECK_EQ(e.pos().line, 2);
    CHECK_EQ(e.pos().column, 3);
  }
}

TEST(incomplete_statement_mentions_the_end) {
  CHECK_ERROR_TEXT(parseScript("SELECT 1 +"), Syntax, "end of input");
  CHECK_ERROR(parseScript("INSERT INTO t VALUES"), Syntax);
  CHECK_ERROR(parseScript("CREATE TABLE t ()"), Syntax);
}

TEST(identifier_quoting_when_printed) {
  CHECK_EQ(ast::quoteIdentifier("plain"), std::string("plain"));
  CHECK_EQ(ast::quoteIdentifier("with space"), std::string("\"with space\""));
  CHECK_EQ(ast::quoteIdentifier("select"), std::string("\"select\""));
  CHECK_EQ(ast::quoteIdentifier("a\"b"), std::string("\"a\"\"b\""));
}

TEST_MAIN
