#include "test_util.h"

using namespace minisql;
using testutil::contains;
using testutil::planText;
using testutil::rowsText;

static std::string q(Database& db, const std::string& sql) { return rowsText(db.query(sql)); }

static Database seeded() {
  Database db;
  db.execute("CREATE TABLE t (id INT PRIMARY KEY, grp INT, name TEXT, score FLOAT);");
  db.execute(
      "INSERT INTO t VALUES (1, 1, 'a', 1.0), (2, 2, 'b', 2.0), (3, 1, 'a', 3.0), (4, NULL, NULL, NULL), (5, 2, 'c', 2.0);");
  return db;
}

TEST(primary_key_lookup_uses_the_index) {
  auto db = seeded();
  CHECK(contains(planText(db, "SELECT name FROM t WHERE id = 3"), "IndexScan t using t_pkey"));
  CHECK_EQ(q(db, "SELECT name FROM t WHERE id = 3"), std::string("a"));
  CHECK_EQ(q(db, "SELECT name FROM t WHERE 3 = id"), std::string("a"));
}

TEST(secondary_index_lifecycle) {
  auto db = seeded();
  CHECK(contains(planText(db, "SELECT id FROM t WHERE grp = 1"), "SeqScan"));
  CHECK_EQ(db.query("CREATE INDEX t_grp ON t (grp)").status, std::string("CREATE INDEX"));
  CHECK(contains(planText(db, "SELECT id FROM t WHERE grp = 1"), "t_grp"));
  CHECK_EQ(q(db, "SELECT id FROM t WHERE grp = 1"), std::string("1;3"));
  db.query("DROP INDEX t_grp");
  CHECK(contains(planText(db, "SELECT id FROM t WHERE grp = 1"), "SeqScan"));
  CHECK_EQ(q(db, "SELECT id FROM t WHERE grp = 1"), std::string("1;3"));
}

TEST(index_stays_correct_through_changes) {
  auto db = seeded();
  db.query("CREATE INDEX t_grp ON t (grp)");
  db.query("UPDATE t SET grp = 9 WHERE id = 1");
  CHECK_EQ(q(db, "SELECT id FROM t WHERE grp = 1"), std::string("3"));
  CHECK_EQ(q(db, "SELECT id FROM t WHERE grp = 9"), std::string("1"));
  db.query("DELETE FROM t WHERE id = 3");
  CHECK_EQ(q(db, "SELECT id FROM t WHERE grp = 1"), std::string(""));
  db.query("INSERT INTO t VALUES (6, 1, 'z', 0.0)");
  CHECK_EQ(q(db, "SELECT id FROM t WHERE grp = 1"), std::string("6"));
}

TEST(index_results_come_in_insertion_order) {
  Database db;
  db.execute("CREATE TABLE t (a INT, b INT); CREATE INDEX t_a ON t (a);");
  db.execute("INSERT INTO t VALUES (1, 1), (2, 2), (1, 3), (1, 4);");
  db.query("UPDATE t SET b = 10 WHERE b = 1");
  CHECK_EQ(q(db, "SELECT b FROM t WHERE a = 1"), q(db, "SELECT b FROM t WHERE a + 0 = 1"));
}

TEST(null_key_finds_nothing) {
  auto db = seeded();
  db.query("CREATE INDEX t_grp ON t (grp)");
  CHECK_EQ(q(db, "SELECT id FROM t WHERE grp = NULL"), std::string(""));
  CHECK_EQ(q(db, "SELECT id FROM t WHERE grp IS NULL"), std::string("4"));
}

TEST(numeric_lookup_across_int_and_float) {
  auto db = seeded();
  db.query("CREATE INDEX t_score ON t (score)");
  CHECK_EQ(q(db, "SELECT id FROM t WHERE id = 3.0"), std::string("3"));
  CHECK_EQ(q(db, "SELECT id FROM t WHERE id = 3.5"), std::string(""));
  CHECK_EQ(q(db, "SELECT id FROM t WHERE score = 2 ORDER BY id"), std::string("2;5"));
}

TEST(index_lookup_with_the_wrong_type_errors_like_a_scan) {
  auto db = seeded();
  db.query("CREATE INDEX t_name ON t (name)");
  CHECK_ERROR(db.query("SELECT id FROM t WHERE id = 'x'"), Type);
  CHECK_ERROR(db.query("SELECT id FROM t WHERE name = 1"), Type);
  CHECK_ERROR(db.query("SELECT id FROM t WHERE grp = 'x'"), Type);
}

TEST(other_conjuncts_stay_as_filters) {
  auto db = seeded();
  db.query("CREATE INDEX t_grp ON t (grp)");
  std::string plan = planText(db, "SELECT id FROM t WHERE grp = 2 AND score > 1 AND name = 'c'");
  CHECK(contains(plan, "using t_grp"));
  CHECK(contains(plan, "filter=(score > 1 AND name = 'c')"));
  CHECK_EQ(q(db, "SELECT id FROM t WHERE grp = 2 AND score > 1 AND name = 'c'"), std::string("5"));
}

TEST(index_not_used_under_or_or_expressions) {
  auto db = seeded();
  CHECK(contains(planText(db, "SELECT id FROM t WHERE id = 1 OR id = 2"), "SeqScan"));
  CHECK(contains(planText(db, "SELECT id FROM t WHERE id + 0 = 1"), "SeqScan"));
  CHECK(contains(planText(db, "SELECT id FROM t WHERE id > 1"), "SeqScan"));
  CHECK(contains(planText(db, "SELECT id FROM t WHERE id = grp"), "SeqScan"));
  CHECK_EQ(q(db, "SELECT id FROM t WHERE id = grp"), std::string("1;2"));
}

TEST(constant_expression_key) {
  auto db = seeded();
  CHECK(contains(planText(db, "SELECT id FROM t WHERE id = 1 + 1"), "key=(1 + 1)"));
  CHECK_EQ(q(db, "SELECT id FROM t WHERE id = 1 + 1"), std::string("2"));
}

TEST(unique_index_enforced) {
  auto db = seeded();
  db.execute("CREATE TABLE u (a INT, b INT); INSERT INTO u VALUES (1, 1), (NULL, 2), (NULL, 3);");
  db.query("CREATE UNIQUE INDEX u_a ON u (a)");
  CHECK_ERROR(db.query("INSERT INTO u VALUES (1, 9)"), Constraint);
  CHECK_ERROR(db.query("UPDATE u SET a = 1 WHERE b = 2"), Constraint);
  db.query("INSERT INTO u VALUES (NULL, 4)");
  CHECK_EQ(q(db, "SELECT COUNT(*) FROM u"), std::string("4"));
}

TEST(unique_index_over_duplicates_fails_and_leaves_no_index) {
  auto db = seeded();
  CHECK_ERROR(db.query("CREATE UNIQUE INDEX bad ON t (grp)"), Constraint);
  CHECK(db.catalog().find("t")->findIndex("bad") == nullptr);
  CHECK(contains(planText(db, "SELECT id FROM t WHERE grp = 1"), "SeqScan"));
}

TEST(index_name_rules) {
  auto db = seeded();
  db.execute("CREATE TABLE u (a INT);");
  db.query("CREATE INDEX i ON t (grp)");
  CHECK_ERROR(db.query("CREATE INDEX i ON u (a)"), Catalog);
  db.query("CREATE INDEX IF NOT EXISTS i ON u (a)");
  CHECK(db.catalog().find("u")->findIndex("i") == nullptr);
  CHECK_ERROR(db.query("CREATE INDEX j ON u (nope)"), Catalog);
  CHECK_ERROR(db.query("CREATE INDEX j ON nope (a)"), Catalog);
  CHECK_ERROR(db.query("DROP INDEX nope"), Catalog);
  db.query("DROP INDEX IF EXISTS nope");
  CHECK_ERROR(db.query("DROP INDEX t_pkey"), Catalog);
  CHECK(db.catalog().find("t")->findIndex("t_pkey") != nullptr);
}

TEST(dropping_a_table_frees_its_index_names) {
  auto db = seeded();
  db.query("CREATE INDEX i ON t (grp)");
  db.query("DROP TABLE t");
  db.execute("CREATE TABLE v (a INT); CREATE INDEX i ON v (a);");
  CHECK(db.catalog().find("v")->findIndex("i") != nullptr);
}

TEST(update_and_delete_use_the_index) {
  auto db = seeded();
  CHECK(contains(planText(db, "UPDATE t SET name = 'x' WHERE id = 1"), "IndexScan t using t_pkey"));
  CHECK(contains(planText(db, "DELETE FROM t WHERE id = 1"), "IndexScan t using t_pkey"));
  db.query("UPDATE t SET name = 'x' WHERE id = 1 AND grp = 1");
  CHECK_EQ(q(db, "SELECT name FROM t WHERE id = 1"), std::string("x"));
  CHECK_EQ(db.query("DELETE FROM t WHERE id = 1 AND grp = 99").affected, size_t{0});
  CHECK_EQ(db.query("DELETE FROM t WHERE id = 1").affected, size_t{1});
}

TEST(index_and_scan_agree_on_a_larger_table) {
  Database db;
  db.execute("CREATE TABLE t (id INT PRIMARY KEY, k INT, s TEXT);");
  std::string insert = "INSERT INTO t VALUES ";
  for (int i = 0; i < 500; ++i) {
    if (i) insert += ", ";
    insert += "(" + std::to_string(i) + ", " + (i % 11 == 0 ? std::string("NULL") : std::to_string(i % 13)) + ", 's" +
              std::to_string(i % 5) + "')";
  }
  db.query(insert);
  db.execute("CREATE INDEX t_k ON t (k); CREATE INDEX t_s ON t (s);");
  for (int k = 0; k < 13; ++k) {
    std::string key = std::to_string(k);
    CHECK_EQ(q(db, "SELECT id FROM t WHERE k = " + key), q(db, "SELECT id FROM t WHERE k + 0 = " + key));
  }
  CHECK_EQ(q(db, "SELECT id FROM t WHERE s = 's3'"), q(db, "SELECT id FROM t WHERE s || '' = 's3'"));
}

TEST_MAIN
