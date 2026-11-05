#include "test_util.h"

using namespace minisql;
using testutil::columnsText;
using testutil::rowsText;

static Database sampleDb() {
  Database db;
  db.execute(
      "CREATE TABLE p (id INT PRIMARY KEY, name TEXT, age INT, score FLOAT, active BOOL);"
      "INSERT INTO p VALUES"
      " (1, 'Ana', 30, 1.5, TRUE),"
      " (2, 'Bo', NULL, 2.5, FALSE),"
      " (3, 'Cy', 25, NULL, TRUE),"
      " (4, NULL, 40, 0.5, NULL);");
  return db;
}

static std::string q(Database& db, const std::string& sql) { return rowsText(db.query(sql)); }

TEST(select_without_from) {
  Database db;
  CHECK_EQ(q(db, "SELECT 1 + 2, 'a' || 'b', NULL, 7 / 2, 7.0 / 2"), std::string("3|ab|NULL|3|3.5"));
  CHECK_EQ(q(db, "SELECT 1 WHERE 1 = 2"), std::string(""));
  CHECK_EQ(q(db, "SELECT 1 WHERE 1 = 1"), std::string("1"));
}

TEST(star_and_column_names) {
  auto db = sampleDb();
  auto result = db.query("SELECT * FROM p WHERE id = 1");
  CHECK_EQ(columnsText(result), std::string("id|name|age|score|active"));
  CHECK_EQ(rowsText(result), std::string("1|Ana|30|1.5|true"));
}

TEST(output_column_naming) {
  auto db = sampleDb();
  auto result = db.query("SELECT id, id + 1, name AS who, UPPER(name) FROM p LIMIT 1");
  CHECK_EQ(columnsText(result), std::string("id|id + 1|who|UPPER(name)"));
}

TEST(names_are_case_insensitive) {
  auto db = sampleDb();
  CHECK_EQ(q(db, "SELECT NAME FROM P WHERE ID = 2"), std::string("Bo"));
}

TEST(where_keeps_only_true_rows) {
  auto db = sampleDb();
  CHECK_EQ(q(db, "SELECT id FROM p WHERE age > 26"), std::string("1;4"));
  CHECK_EQ(q(db, "SELECT id FROM p WHERE NOT age > 26"), std::string("3"));
  CHECK_EQ(q(db, "SELECT id FROM p WHERE active"), std::string("1;3"));
}

TEST(three_valued_and_or) {
  Database db;
  CHECK_EQ(q(db, "SELECT NULL AND FALSE, NULL AND TRUE, NULL OR TRUE, NULL OR FALSE, NOT NULL"),
           std::string("false|NULL|true|NULL|NULL"));
}

TEST(and_or_short_circuit_on_the_deciding_value) {
  Database db;
  CHECK_EQ(q(db, "SELECT FALSE AND 1 / 0 = 1, TRUE OR 1 / 0 = 1"), std::string("false|true"));
}

TEST(is_null) {
  auto db = sampleDb();
  CHECK_EQ(q(db, "SELECT id FROM p WHERE age IS NULL"), std::string("2"));
  CHECK_EQ(q(db, "SELECT id FROM p WHERE age IS NOT NULL"), std::string("1;3;4"));
  CHECK_EQ(q(db, "SELECT NULL IS NULL, 1 IS NULL"), std::string("true|false"));
}

TEST(equals_null_matches_nothing) {
  auto db = sampleDb();
  CHECK_EQ(q(db, "SELECT id FROM p WHERE age = NULL"), std::string(""));
  CHECK_EQ(q(db, "SELECT id FROM p WHERE age <> NULL"), std::string(""));
}

TEST(in_list_three_valued) {
  Database db;
  CHECK_EQ(q(db, "SELECT 1 IN (1, 2), 3 IN (1, 2), 1 IN (1, NULL), 3 IN (1, NULL), NULL IN (1)"),
           std::string("true|false|true|NULL|NULL"));
  CHECK_EQ(q(db, "SELECT 1 NOT IN (1, 2), 3 NOT IN (1, 2), 3 NOT IN (1, NULL)"), std::string("false|true|NULL"));
}

TEST(not_in_with_null_keeps_no_rows) {
  auto db = sampleDb();
  CHECK_EQ(q(db, "SELECT id FROM p WHERE id NOT IN (1, NULL)"), std::string(""));
  CHECK_EQ(q(db, "SELECT id FROM p WHERE id NOT IN (1, 2)"), std::string("3;4"));
}

TEST(between_three_valued) {
  Database db;
  CHECK_EQ(q(db, "SELECT 2 BETWEEN 1 AND 3, 5 BETWEEN 1 AND 3, 2 NOT BETWEEN 1 AND 3"),
           std::string("true|false|false"));
  CHECK_EQ(q(db, "SELECT NULL BETWEEN 1 AND 3, 2 BETWEEN NULL AND 3, 5 BETWEEN NULL AND 3, 2 BETWEEN 3 AND NULL"),
           std::string("NULL|NULL|false|false"));
}

TEST(like_in_queries) {
  auto db = sampleDb();
  CHECK_EQ(q(db, "SELECT id FROM p WHERE name LIKE 'A%'"), std::string("1"));
  CHECK_EQ(q(db, "SELECT id FROM p WHERE name NOT LIKE 'A%'"), std::string("2;3"));
  CHECK_EQ(q(db, "SELECT NULL LIKE 'a', 'a' LIKE NULL"), std::string("NULL|NULL"));
  CHECK_ERROR(db.query("SELECT 1 LIKE 'a'"), Type);
}

TEST(scalar_functions) {
  Database db;
  CHECK_EQ(q(db, "SELECT UPPER('aB'), LOWER('aB'), LENGTH('abc'), ABS(-2), ABS(-2.5)"),
           std::string("AB|ab|3|2|2.5"));
  CHECK_EQ(q(db, "SELECT UPPER(NULL), LOWER(NULL), LENGTH(NULL), ABS(NULL)"), std::string("NULL|NULL|NULL|NULL"));
  CHECK_ERROR(db.query("SELECT LENGTH(1)"), Type);
  CHECK_ERROR(db.query("SELECT ABS('x')"), Type);
  CHECK_ERROR(db.query("SELECT ABS(-9223372036854775808)"), Arithmetic);
}

TEST(function_argument_counts_and_names) {
  Database db;
  CHECK_ERROR(db.query("SELECT UPPER()"), Syntax);
  CHECK_ERROR(db.query("SELECT UPPER('a', 'b')"), Syntax);
  CHECK_ERROR(db.query("SELECT NO_SUCH_FUNCTION(1)"), Catalog);
  CHECK_ERROR(db.query("SELECT COALESCE()"), Syntax);
}

TEST(coalesce_stops_at_the_first_value) {
  Database db;
  CHECK_EQ(q(db, "SELECT COALESCE(NULL, 2, 3), COALESCE(NULL, NULL)"), std::string("2|NULL"));
  // Later arguments are not evaluated once a value is found.
  CHECK_EQ(q(db, "SELECT COALESCE(1, 1 / 0)"), std::string("1"));
  CHECK_ERROR(db.query("SELECT COALESCE(NULL, 1 / 0)"), Arithmetic);
}

TEST(cast_expression) {
  Database db;
  CHECK_EQ(q(db, "SELECT CAST('42' AS INT) + 1, CAST(3 AS TEXT) || 'x', CAST(1 AS FLOAT)"), std::string("43|3x|1.0"));
}

TEST(order_by_directions_and_nulls) {
  auto db = sampleDb();
  CHECK_EQ(q(db, "SELECT id FROM p ORDER BY age"), std::string("2;3;1;4"));
  CHECK_EQ(q(db, "SELECT id FROM p ORDER BY age DESC"), std::string("4;1;3;2"));
}

TEST(order_by_is_stable_and_multi_key) {
  Database db;
  db.execute("CREATE TABLE t (a INT, b INT); INSERT INTO t VALUES (1, 1), (2, 2), (1, 3), (2, 4), (1, 5);");
  CHECK_EQ(q(db, "SELECT b FROM t ORDER BY a"), std::string("1;3;5;2;4"));
  CHECK_EQ(q(db, "SELECT b FROM t ORDER BY a DESC, b DESC"), std::string("4;2;5;3;1"));
}

TEST(order_by_alias_position_and_expression) {
  auto db = sampleDb();
  CHECK_EQ(q(db, "SELECT id, 0 - id AS neg FROM p ORDER BY neg"), std::string("4|-4;3|-3;2|-2;1|-1"));
  CHECK_EQ(q(db, "SELECT id, 0 - id FROM p ORDER BY 2"), std::string("4|-4;3|-3;2|-2;1|-1"));
  CHECK_EQ(q(db, "SELECT id FROM p ORDER BY 0 - id"), std::string("4;3;2;1"));
  CHECK_EQ(q(db, "SELECT * FROM p ORDER BY 1 DESC LIMIT 1"), std::string("4|NULL|40|0.5|NULL"));
  CHECK_ERROR(db.query("SELECT id FROM p ORDER BY 2"), Syntax);
  CHECK_ERROR(db.query("SELECT id FROM p ORDER BY 0"), Syntax);
}

TEST(order_by_column_not_in_the_output) {
  auto db = sampleDb();
  CHECK_EQ(q(db, "SELECT name FROM p WHERE name IS NOT NULL ORDER BY age DESC"), std::string("Ana;Cy;Bo"));
}

TEST(comparing_a_column_to_the_wrong_type_is_an_error) {
  Database db;
  db.execute("CREATE TABLE t (a INT, b TEXT); INSERT INTO t VALUES (1, 'x'), (2, 'y');");
  CHECK_ERROR(db.query("SELECT a FROM t WHERE a = 'x'"), Type);
}

TEST(limit_and_offset) {
  auto db = sampleDb();
  CHECK_EQ(q(db, "SELECT id FROM p ORDER BY id LIMIT 2"), std::string("1;2"));
  CHECK_EQ(q(db, "SELECT id FROM p ORDER BY id LIMIT 2 OFFSET 1"), std::string("2;3"));
  CHECK_EQ(q(db, "SELECT id FROM p ORDER BY id LIMIT 2 OFFSET 3"), std::string("4"));
  CHECK_EQ(q(db, "SELECT id FROM p ORDER BY id LIMIT 2 OFFSET 99"), std::string(""));
  CHECK_EQ(q(db, "SELECT id FROM p LIMIT 0"), std::string(""));
  CHECK_EQ(q(db, "SELECT id FROM p ORDER BY id LIMIT 1 + 1"), std::string("1;2"));
}

TEST(bad_limits) {
  auto db = sampleDb();
  CHECK_ERROR(db.query("SELECT id FROM p LIMIT -1"), Type);
  CHECK_ERROR(db.query("SELECT id FROM p LIMIT 'a'"), Type);
  CHECK_ERROR(db.query("SELECT id FROM p LIMIT 1.5"), Type);
  CHECK_ERROR(db.query("SELECT id FROM p LIMIT id"), Syntax);
  CHECK_ERROR(db.query("SELECT id FROM p LIMIT 1 OFFSET -1"), Type);
}

TEST(unknown_names) {
  auto db = sampleDb();
  CHECK_ERROR(db.query("SELECT nope FROM p"), Catalog);
  CHECK_ERROR(db.query("SELECT p.nope FROM p"), Catalog);
  CHECK_ERROR(db.query("SELECT x.id FROM p"), Catalog);
  CHECK_ERROR(db.query("SELECT id FROM nowhere"), Catalog);
  CHECK_ERROR(db.query("SELECT id"), Catalog);
  CHECK_ERROR(db.query("SELECT x.* FROM p"), Catalog);
  CHECK_ERROR(db.query("SELECT *"), Syntax);
}

TEST(unknown_names_fail_even_on_empty_tables) {
  Database db;
  db.execute("CREATE TABLE t (a INT);");
  CHECK_ERROR(db.query("SELECT nope FROM t"), Catalog);
  CHECK_ERROR(db.query("SELECT a FROM t WHERE nope = 1"), Catalog);
  CHECK_ERROR(db.query("SELECT a FROM t ORDER BY nope"), Catalog);
}

TEST(where_must_be_boolean) {
  auto db = sampleDb();
  CHECK_ERROR(db.query("SELECT id FROM p WHERE id"), Type);
  CHECK_ERROR(db.query("SELECT id FROM p WHERE name"), Type);
  CHECK_ERROR(db.query("SELECT 1 WHERE 'x' AND TRUE"), Type);
}

TEST(aggregates_are_not_allowed_in_where) {
  auto db = sampleDb();
  CHECK_ERROR(db.query("SELECT id FROM p WHERE COUNT(*) > 1"), Syntax);
}

TEST(quoted_identifiers_as_names) {
  Database db;
  db.execute("CREATE TABLE \"order\" (\"group\" INT, \"two words\" TEXT);"
             "INSERT INTO \"order\" VALUES (1, 'x');");
  CHECK_EQ(q(db, "SELECT \"group\", \"two words\" FROM \"order\""), std::string("1|x"));
}

TEST(select_from_empty_table) {
  Database db;
  db.execute("CREATE TABLE t (a INT);");
  auto result = db.query("SELECT a, a + 1 FROM t ORDER BY a LIMIT 3");
  CHECK_EQ(result.rows.size(), size_t{0});
  CHECK_EQ(columnsText(result), std::string("a|a + 1"));
}

TEST_MAIN
