#include "test_util.h"

using namespace minisql;
using testutil::contains;
using testutil::planText;

static Database seeded() {
  Database db;
  db.execute(
      "CREATE TABLE a (id INT PRIMARY KEY, x INT);"
      "CREATE TABLE b (id INT PRIMARY KEY, a_id INT);"
      "INSERT INTO a VALUES (1, 1);");
  return db;
}

TEST(simple_scan_plan) {
  auto db = seeded();
  CHECK_EQ(planText(db, "SELECT x FROM a"), std::string("Project [x]\n  SeqScan a\n"));
  CHECK_EQ(planText(db, "SELECT x FROM a WHERE x > 0"), std::string("Project [x]\n  SeqScan a filter=(x > 0)\n"));
}

TEST(select_without_table) {
  auto db = seeded();
  CHECK_EQ(planText(db, "SELECT 1"), std::string("Project [1]\n  SingleRow\n"));
  CHECK_EQ(planText(db, "SELECT 1 WHERE 1 = 2"), std::string("Project [1]\n  Filter (1 = 2)\n    SingleRow\n"));
}

TEST(full_pipeline_order) {
  auto db = seeded();
  std::string plan = planText(db, "SELECT x, COUNT(*) AS n FROM a GROUP BY x HAVING COUNT(*) > 0 ORDER BY n DESC LIMIT 3 OFFSET 1");
  CHECK_EQ(plan, std::string("Project [x, n]\n"
                             "  Limit 3 offset 1\n"
                             "    Sort [COUNT(*) DESC]\n"
                             "      Filter (COUNT(*) > 0)\n"
                             "        HashAggregate keys=[x] aggregates=[COUNT(*)]\n"
                             "          SeqScan a\n"));
}

TEST(limit_without_offset) {
  auto db = seeded();
  CHECK(contains(planText(db, "SELECT x FROM a LIMIT 1"), "Limit 1\n"));
}

TEST(join_plan_shape) {
  auto db = seeded();
  CHECK_EQ(planText(db, "SELECT a.x FROM b JOIN a ON a.id = b.a_id"),
           std::string("Project [x]\n"
                       "  IndexNestedLoopJoin\n"
                       "    SeqScan b\n"
                       "    IndexScan a using a_pkey key=(b.a_id)\n"));
}

TEST(residual_join_condition_is_shown) {
  auto db = seeded();
  std::string plan = planText(db, "SELECT 1 FROM b JOIN a ON a.id = b.a_id AND a.x > b.id");
  CHECK(contains(plan, "IndexNestedLoopJoin on=(a.x > b.id)"));
}

TEST(dml_plans) {
  auto db = seeded();
  CHECK_EQ(planText(db, "UPDATE a SET x = 1 WHERE id = 1"), std::string("Update a\n  IndexScan a using a_pkey key=(1)\n"));
  CHECK_EQ(planText(db, "DELETE FROM a"), std::string("Delete a\n  SeqScan a\n"));
  CHECK_EQ(planText(db, "INSERT INTO a VALUES (5, 5), (6, 6)"), std::string("Insert a rows=2\n"));
}

TEST(explain_runs_nothing) {
  auto db = seeded();
  db.query("EXPLAIN DELETE FROM a");
  db.query("EXPLAIN UPDATE a SET x = 100");
  db.query("EXPLAIN INSERT INTO a VALUES (7, 7)");
  CHECK_EQ(testutil::rowsText(db.query("SELECT id, x FROM a")), std::string("1|1"));
}

TEST(explain_still_checks_names) {
  auto db = seeded();
  CHECK_ERROR(db.query("EXPLAIN SELECT nope FROM a"), Catalog);
  CHECK_ERROR(db.query("EXPLAIN UPDATE a SET nope = 1"), Catalog);
  CHECK_ERROR(db.query("EXPLAIN DELETE FROM nope"), Catalog);
  CHECK_ERROR(db.query("EXPLAIN INSERT INTO nope VALUES (1)"), Catalog);
}

TEST(explain_returns_a_single_plan_column) {
  auto db = seeded();
  auto result = db.query("EXPLAIN SELECT x FROM a");
  CHECK_EQ(testutil::columnsText(result), std::string("plan"));
  CHECK_EQ(result.rows.size(), size_t{2});
}

TEST_MAIN
