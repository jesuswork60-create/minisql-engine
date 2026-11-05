#include "test_util.h"

using namespace minisql;
using testutil::columnsText;
using testutil::rowsText;

static std::string q(Database& db, const std::string& sql) { return rowsText(db.query(sql)); }

static Database seeded() {
  Database db;
  db.execute(
      "CREATE TABLE s (region TEXT, item TEXT, qty INT, price FLOAT);"
      "INSERT INTO s VALUES"
      " ('n', 'a', 1, 1.5),"
      " ('n', 'b', 2, NULL),"
      " ('s', 'a', 3, 2.5),"
      " ('s', 'a', NULL, 0.5),"
      " (NULL, 'c', 5, 4.0);");
  return db;
}

TEST(aggregates_over_the_whole_table) {
  auto db = seeded();
  CHECK_EQ(q(db, "SELECT COUNT(*), COUNT(qty), SUM(qty), AVG(qty), MIN(qty), MAX(qty) FROM s"),
           std::string("5|4|11|2.75|1|5"));
  CHECK_EQ(q(db, "SELECT SUM(price), MIN(item), MAX(item) FROM s"), std::string("8.5|a|c"));
}

TEST(aggregates_over_nothing) {
  auto db = seeded();
  CHECK_EQ(q(db, "SELECT COUNT(*), COUNT(qty), SUM(qty), AVG(qty), MIN(qty), MAX(qty) FROM s WHERE qty > 100"),
           std::string("0|0|NULL|NULL|NULL|NULL"));
  CHECK_EQ(q(db, "SELECT region, COUNT(*) FROM s WHERE qty > 100 GROUP BY region"), std::string(""));
}

TEST(aggregates_skip_null_inputs_entirely) {
  Database db;
  db.execute("CREATE TABLE t (a INT); INSERT INTO t VALUES (NULL), (NULL);");
  CHECK_EQ(q(db, "SELECT COUNT(*), COUNT(a), SUM(a), AVG(a), MIN(a), MAX(a) FROM t"),
           std::string("2|0|NULL|NULL|NULL|NULL"));
}

TEST(group_by_with_null_group) {
  auto db = seeded();
  CHECK_EQ(q(db, "SELECT region, COUNT(*), SUM(qty) FROM s GROUP BY region ORDER BY region"),
           std::string("NULL|1|5;n|2|3;s|2|3"));
}

TEST(groups_come_out_in_first_seen_order) {
  auto db = seeded();
  CHECK_EQ(q(db, "SELECT item FROM s GROUP BY item"), std::string("a;b;c"));
}

TEST(group_by_several_keys) {
  auto db = seeded();
  CHECK_EQ(q(db, "SELECT region, item, COUNT(*) FROM s GROUP BY region, item ORDER BY region, item"),
           std::string("NULL|c|1;n|a|1;n|b|1;s|a|2"));
}

TEST(group_by_expression_matches_the_same_expression_in_select) {
  auto db = seeded();
  CHECK_EQ(q(db, "SELECT qty % 2, COUNT(*) FROM s WHERE qty IS NOT NULL GROUP BY qty % 2 ORDER BY qty % 2"),
           std::string("0|1;1|3"));
  CHECK_EQ(q(db, "SELECT UPPER(item) || '!' FROM s GROUP BY UPPER(item) ORDER BY 1"), std::string("A!;B!;C!"));
}

TEST(group_key_matching_ignores_qualifier_and_case) {
  auto db = seeded();
  CHECK_EQ(q(db, "SELECT s.REGION, COUNT(*) FROM s WHERE region = 'n' GROUP BY region"), std::string("n|2"));
  CHECK_EQ(q(db, "SELECT region FROM s WHERE region = 'n' GROUP BY s.region"), std::string("n"));
}

TEST(int_and_float_keys_share_a_group) {
  Database db;
  db.execute("CREATE TABLE t (f FLOAT, n INT); INSERT INTO t VALUES (1, 1), (1.0, 2), (2.5, 3);");
  CHECK_EQ(q(db, "SELECT f, COUNT(*) FROM t GROUP BY f"), std::string("1.0|2;2.5|1"));
}

TEST(ungrouped_columns_are_rejected) {
  auto db = seeded();
  CHECK_ERROR_TEXT(db.query("SELECT item, COUNT(*) FROM s"), Syntax, "item");
  CHECK_ERROR(db.query("SELECT item FROM s GROUP BY region"), Syntax);
  CHECK_ERROR(db.query("SELECT qty + 1 FROM s GROUP BY qty + 2"), Syntax);
  CHECK_ERROR(db.query("SELECT region, COUNT(*) FROM s GROUP BY region ORDER BY item"), Syntax);
  CHECK_ERROR(db.query("SELECT region FROM s GROUP BY region HAVING item = 'a'"), Syntax);
}

TEST(star_cannot_mix_with_aggregation) {
  auto db = seeded();
  CHECK_ERROR(db.query("SELECT * FROM s GROUP BY region"), Syntax);
  CHECK_ERROR(db.query("SELECT *, COUNT(*) FROM s"), Syntax);
}

TEST(aggregates_in_expressions) {
  auto db = seeded();
  CHECK_EQ(q(db, "SELECT SUM(qty) * 2 + COUNT(*), MAX(qty) - MIN(qty) FROM s"), std::string("27|4"));
  CHECK_EQ(q(db, "SELECT SUM(qty * 2) FROM s"), std::string("22"));
  CHECK_EQ(q(db, "SELECT COALESCE(SUM(qty), 0) FROM s WHERE qty > 100"), std::string("0"));
}

TEST(same_aggregate_used_twice_is_one_computation) {
  auto db = seeded();
  CHECK_EQ(q(db, "SELECT COUNT(*), COUNT(*) + 1 FROM s"), std::string("5|6"));
  CHECK(testutil::contains(testutil::planText(db, "SELECT COUNT(*), COUNT(*) + 1 FROM s"), "aggregates=[COUNT(*)]"));
}

TEST(nested_aggregates_and_wrong_arguments) {
  auto db = seeded();
  CHECK_ERROR(db.query("SELECT SUM(COUNT(*)) FROM s"), Syntax);
  CHECK_ERROR(db.query("SELECT SUM(*) FROM s"), Syntax);
  CHECK_ERROR(db.query("SELECT SUM() FROM s"), Syntax);
  CHECK_ERROR(db.query("SELECT COUNT() FROM s"), Syntax);
  CHECK_ERROR(db.query("SELECT SUM(qty, price) FROM s"), Syntax);
  CHECK_ERROR(db.query("SELECT UPPER(*) FROM s"), Syntax);
  CHECK_ERROR(db.query("SELECT SUM(nope) FROM s"), Catalog);
}

TEST(aggregate_type_errors) {
  auto db = seeded();
  CHECK_ERROR(db.query("SELECT SUM(item) FROM s"), Type);
  CHECK_ERROR(db.query("SELECT AVG(item) FROM s"), Type);
}

TEST(sum_overflow_is_reported) {
  Database db;
  db.execute("CREATE TABLE t (a INT); INSERT INTO t VALUES (9223372036854775807), (1);");
  CHECK_ERROR(db.query("SELECT SUM(a) FROM t"), Arithmetic);
}

TEST(sum_keeps_int_type_and_avg_is_float) {
  Database db;
  db.execute("CREATE TABLE t (a INT, f FLOAT); INSERT INTO t VALUES (1, 1.5), (2, 2.5);");
  auto result = db.query("SELECT SUM(a), SUM(f), AVG(a) FROM t");
  CHECK(result.rows[0][0].isInt());
  CHECK(result.rows[0][1].isFloat());
  CHECK(result.rows[0][2].isFloat());
}

TEST(having_filters_groups) {
  auto db = seeded();
  CHECK_EQ(q(db, "SELECT region FROM s GROUP BY region HAVING COUNT(*) > 1 ORDER BY region"), std::string("n;s"));
  CHECK_EQ(q(db, "SELECT region, COUNT(*) FROM s GROUP BY region HAVING SUM(qty) > 3"), std::string("NULL|1"));
  CHECK_EQ(q(db, "SELECT region FROM s GROUP BY region HAVING region = 'n'"), std::string("n"));
}

TEST(having_without_group_by_treats_the_table_as_one_group) {
  auto db = seeded();
  CHECK_EQ(q(db, "SELECT COUNT(*) FROM s HAVING COUNT(*) > 1"), std::string("5"));
  CHECK_EQ(q(db, "SELECT COUNT(*) FROM s HAVING COUNT(*) > 100"), std::string(""));
  CHECK_EQ(q(db, "SELECT 1 FROM s HAVING COUNT(*) = 5"), std::string("1"));
}

TEST(having_must_be_boolean) {
  auto db = seeded();
  CHECK_ERROR(db.query("SELECT region FROM s GROUP BY region HAVING COUNT(*)"), Type);
}

TEST(order_by_aggregates) {
  auto db = seeded();
  CHECK_EQ(q(db, "SELECT item FROM s GROUP BY item ORDER BY COUNT(*) DESC, item"), std::string("a;b;c"));
  CHECK_EQ(q(db, "SELECT item, SUM(qty) AS total FROM s GROUP BY item ORDER BY total DESC"),
           std::string("c|5;a|4;b|2"));
  CHECK_EQ(q(db, "SELECT item, SUM(qty) FROM s GROUP BY item ORDER BY 2"), std::string("b|2;a|4;c|5"));
  CHECK_EQ(q(db, "SELECT item FROM s GROUP BY item ORDER BY SUM(qty) LIMIT 1"), std::string("b"));
}

TEST(order_by_aggregate_alone_makes_it_an_aggregate_query) {
  auto db = seeded();
  CHECK_EQ(q(db, "SELECT 1 FROM s ORDER BY COUNT(*)"), std::string("1"));
}

TEST(column_names_for_aggregates) {
  auto db = seeded();
  CHECK_EQ(columnsText(db.query("SELECT COUNT(*), sum(qty) AS total, MAX(qty) FROM s")),
           std::string("COUNT(*)|total|MAX(qty)"));
}

TEST(aggregate_without_from) {
  Database db;
  CHECK_EQ(q(db, "SELECT COUNT(*), SUM(5)"), std::string("1|5"));
}

TEST(limit_after_grouping) {
  auto db = seeded();
  CHECK_EQ(q(db, "SELECT item, COUNT(*) FROM s GROUP BY item ORDER BY item LIMIT 1 OFFSET 1"), std::string("b|1"));
}

TEST(lower_case_function_names) {
  auto db = seeded();
  CHECK_EQ(q(db, "select count(*), Upper('x') from s"), std::string("5|X"));
}

TEST_MAIN
