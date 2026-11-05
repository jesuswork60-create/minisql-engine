#include "test_util.h"

using namespace minisql;
using testutil::columnsText;
using testutil::contains;
using testutil::planText;
using testutil::rowsText;

static std::string q(Database& db, const std::string& sql) { return rowsText(db.query(sql)); }

static Database seeded() {
  Database db;
  db.execute(
      "CREATE TABLE users (id INT PRIMARY KEY, name TEXT, city_id INT);"
      "CREATE TABLE orders (id INT PRIMARY KEY, user_id INT, total INT);"
      "CREATE TABLE cities (id INT PRIMARY KEY, label TEXT);"
      "INSERT INTO users VALUES (1, 'Ana', 1), (2, 'Bo', 2), (3, 'Cy', NULL);"
      "INSERT INTO orders VALUES (10, 1, 5), (11, 1, 7), (12, 2, 9), (13, NULL, 1), (14, 99, 2);"
      "INSERT INTO cities VALUES (1, 'Lima'), (2, 'Oslo');");
  return db;
}

TEST(inner_join_with_aliases) {
  auto db = seeded();
  CHECK_EQ(q(db, "SELECT u.name, o.total FROM users u JOIN orders o ON o.user_id = u.id ORDER BY o.id"),
           std::string("Ana|5;Ana|7;Bo|9"));
  CHECK_EQ(q(db, "SELECT u.name, o.total FROM users AS u INNER JOIN orders AS o ON u.id = o.user_id ORDER BY o.id"),
           std::string("Ana|5;Ana|7;Bo|9"));
}

TEST(null_join_keys_never_match) {
  auto db = seeded();
  CHECK_EQ(q(db, "SELECT COUNT(*) FROM orders o JOIN users u ON o.user_id = u.id"), std::string("3"));
}

TEST(three_way_join) {
  auto db = seeded();
  CHECK_EQ(q(db,
             "SELECT u.name, c.label, o.total FROM users u "
             "JOIN orders o ON o.user_id = u.id "
             "JOIN cities c ON c.id = u.city_id "
             "ORDER BY o.total DESC"),
           std::string("Bo|Oslo|9;Ana|Lima|7;Ana|Lima|5"));
}

TEST(star_across_a_join) {
  auto db = seeded();
  auto result = db.query("SELECT * FROM cities c JOIN users u ON u.city_id = c.id ORDER BY u.id LIMIT 1");
  CHECK_EQ(columnsText(result), std::string("id|label|id|name|city_id"));
  CHECK_EQ(rowsText(result), std::string("1|Lima|1|Ana|1"));
  CHECK_EQ(columnsText(db.query("SELECT u.* FROM cities c JOIN users u ON u.city_id = c.id")),
           std::string("id|name|city_id"));
}

TEST(join_plan_uses_the_index_on_the_inner_table) {
  auto db = seeded();
  std::string plan = planText(db, "SELECT u.name FROM orders o JOIN users u ON o.user_id = u.id");
  CHECK(contains(plan, "IndexNestedLoopJoin"));
  CHECK(contains(plan, "IndexScan users AS u using users_pkey key=(o.user_id)"));
}

TEST(join_without_an_index_scans_and_gives_the_same_rows) {
  auto db = seeded();
  std::string indexed = q(db, "SELECT u.name, o.total FROM orders o JOIN users u ON o.user_id = u.id ORDER BY o.id");
  std::string plan = planText(db, "SELECT u.name, o.total FROM users u JOIN orders o ON o.user_id = u.id");
  CHECK(contains(plan, "NestedLoopJoin on=(o.user_id = u.id)"));
  CHECK(!contains(plan, "IndexNestedLoopJoin"));
  CHECK_EQ(q(db, "SELECT u.name, o.total FROM users u JOIN orders o ON o.user_id = u.id ORDER BY o.id"), indexed);
}

TEST(index_join_with_mismatched_key_type_reports_it) {
  Database db;
  db.execute(
      "CREATE TABLE a (k TEXT); CREATE TABLE b (k INT PRIMARY KEY);"
      "INSERT INTO a VALUES ('x'); INSERT INTO b VALUES (1);");
  CHECK_ERROR(db.query("SELECT * FROM a JOIN b ON a.k = b.k"), Type);
}

TEST(index_join_with_null_outer_key_is_skipped) {
  auto db = seeded();
  CHECK_EQ(q(db, "SELECT o.id FROM orders o JOIN users u ON u.id = o.user_id WHERE o.user_id IS NULL"), std::string(""));
}

TEST(where_terms_are_pushed_down) {
  auto db = seeded();
  std::string plan = planText(db, "SELECT u.name FROM users u JOIN orders o ON o.user_id = u.id WHERE u.name = 'Bo' AND o.total > 1");
  CHECK(contains(plan, "SeqScan users AS u filter=(u.name = 'Bo')"));
  CHECK(contains(plan, "SeqScan orders AS o filter=(o.total > 1)"));
  CHECK_EQ(q(db, "SELECT u.name FROM users u JOIN orders o ON o.user_id = u.id WHERE u.name = 'Bo' AND o.total > 1"),
           std::string("Bo"));
}

TEST(pushed_filter_refers_to_the_right_columns_of_a_later_table) {
  // A pushed term must be read against the inner table's own row, not the joined row.
  auto db = seeded();
  CHECK_EQ(q(db, "SELECT o.id FROM users u JOIN orders o ON o.user_id = u.id WHERE o.id = 12"), std::string("12"));
  CHECK_EQ(q(db, "SELECT c.label FROM users u JOIN cities c ON c.id = u.city_id WHERE c.label = 'Oslo'"),
           std::string("Oslo"));
}

TEST(comma_and_cross_join) {
  auto db = seeded();
  CHECK_EQ(q(db, "SELECT COUNT(*) FROM users, cities"), std::string("6"));
  CHECK_EQ(q(db, "SELECT COUNT(*) FROM users CROSS JOIN cities"), std::string("6"));
  CHECK_EQ(q(db, "SELECT u.name FROM users u, cities c WHERE u.city_id = c.id AND c.label = 'Lima'"),
           std::string("Ana"));
}

TEST(self_join_needs_and_uses_aliases) {
  Database db;
  db.execute("CREATE TABLE emp (id INT PRIMARY KEY, boss INT, name TEXT);"
             "INSERT INTO emp VALUES (1, NULL, 'Root'), (2, 1, 'Kit'), (3, 2, 'Lee');");
  CHECK_EQ(q(db, "SELECT e.name, b.name FROM emp e JOIN emp b ON e.boss = b.id ORDER BY e.id"),
           std::string("Kit|Root;Lee|Kit"));
  CHECK_ERROR(db.query("SELECT * FROM emp JOIN emp ON 1 = 1"), Catalog);
  CHECK_ERROR(db.query("SELECT * FROM emp a JOIN emp a ON 1 = 1"), Catalog);
}

TEST(ambiguous_and_unknown_columns) {
  auto db = seeded();
  CHECK_ERROR_TEXT(db.query("SELECT id FROM users u JOIN orders o ON o.user_id = u.id"), Catalog, "ambiguous");
  CHECK_ERROR(db.query("SELECT u.total FROM users u JOIN orders o ON o.user_id = u.id"), Catalog);
  CHECK_ERROR(db.query("SELECT 1 FROM users u JOIN orders o ON o.nope = u.id"), Catalog);
  CHECK_EQ(q(db, "SELECT name FROM users u JOIN orders o ON o.user_id = u.id WHERE total = 9"), std::string("Bo"));
}

TEST(on_clause_only_sees_tables_joined_so_far) {
  auto db = seeded();
  CHECK_ERROR(db.query("SELECT 1 FROM users u JOIN orders o ON o.user_id = c.id JOIN cities c ON c.id = u.city_id"),
              Catalog);
}

TEST(original_table_name_is_hidden_by_an_alias) {
  auto db = seeded();
  CHECK_ERROR(db.query("SELECT users.name FROM users u"), Catalog);
}

TEST(on_must_be_boolean) {
  auto db = seeded();
  CHECK_ERROR(db.query("SELECT 1 FROM users u JOIN cities c ON u.id"), Type);
}

TEST(non_equality_join) {
  auto db = seeded();
  CHECK_EQ(q(db, "SELECT u.id, c.id FROM users u JOIN cities c ON u.id < c.id ORDER BY u.id, c.id"), std::string("1|2"));
}

TEST(join_with_outer_table_in_an_expression_key) {
  auto db = seeded();
  std::string plan = planText(db, "SELECT o.id FROM users u JOIN orders o ON u.id = o.id - 9");
  CHECK(contains(plan, "SeqScan orders"));
  CHECK_EQ(q(db, "SELECT o.id FROM users u JOIN orders o ON u.id = o.id - 9 ORDER BY o.id"), std::string("10;11;12"));
  std::string keyed = planText(db, "SELECT u.id FROM orders o JOIN users u ON u.id = o.id - 9");
  CHECK(contains(keyed, "key=(o.id - 9)"));
  CHECK_EQ(q(db, "SELECT u.id FROM orders o JOIN users u ON u.id = o.id - 9 ORDER BY o.id"), std::string("1;2;3"));
}

TEST(empty_outer_or_inner) {
  auto db = seeded();
  db.execute("CREATE TABLE none (id INT PRIMARY KEY);");
  CHECK_EQ(q(db, "SELECT COUNT(*) FROM none n JOIN users u ON u.id = n.id"), std::string("0"));
  CHECK_EQ(q(db, "SELECT COUNT(*) FROM users u JOIN none n ON u.id = n.id"), std::string("0"));
}

TEST(joins_with_group_by_and_order) {
  auto db = seeded();
  CHECK_EQ(q(db,
             "SELECT u.name, SUM(o.total) AS spent FROM users u JOIN orders o ON o.user_id = u.id "
             "GROUP BY u.name ORDER BY spent DESC"),
           std::string("Ana|12;Bo|9"));
}

TEST_MAIN
