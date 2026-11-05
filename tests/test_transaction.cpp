#include "test_util.h"

using namespace minisql;
using testutil::rowsText;

static std::string q(Database& db, const std::string& sql) { return rowsText(db.query(sql)); }

static Database seeded() {
  Database db;
  db.execute("CREATE TABLE t (id INT PRIMARY KEY, v INT); INSERT INTO t VALUES (1, 10), (2, 20);");
  return db;
}

TEST(commit_keeps_changes) {
  auto db = seeded();
  db.execute("BEGIN; INSERT INTO t VALUES (3, 30); COMMIT;");
  CHECK(!db.inTransaction());
  CHECK_EQ(q(db, "SELECT id FROM t ORDER BY id"), std::string("1;2;3"));
}

TEST(rollback_undoes_every_kind_of_row_change) {
  auto db = seeded();
  db.execute("BEGIN; INSERT INTO t VALUES (3, 30); UPDATE t SET v = 0 WHERE id = 1; DELETE FROM t WHERE id = 2;");
  CHECK(db.inTransaction());
  CHECK_EQ(q(db, "SELECT id, v FROM t ORDER BY id"), std::string("1|0;3|30"));
  db.query("ROLLBACK");
  CHECK(!db.inTransaction());
  CHECK_EQ(q(db, "SELECT id, v FROM t ORDER BY id"), std::string("1|10;2|20"));
}

TEST(rollback_restores_indexes) {
  auto db = seeded();
  db.execute("BEGIN; DELETE FROM t WHERE id = 1; INSERT INTO t VALUES (3, 30); ROLLBACK;");
  CHECK_EQ(q(db, "SELECT v FROM t WHERE id = 1"), std::string("10"));
  CHECK_EQ(q(db, "SELECT v FROM t WHERE id = 3"), std::string(""));
  CHECK_ERROR(db.query("INSERT INTO t VALUES (1, 0)"), Constraint);
  db.query("INSERT INTO t VALUES (3, 0)");
}

TEST(rollback_undoes_schema_changes) {
  auto db = seeded();
  db.execute(
      "BEGIN;"
      "CREATE TABLE fresh (a INT);"
      "CREATE INDEX t_v ON t (v);"
      "DROP TABLE t;"
      "ROLLBACK;");
  CHECK(db.catalog().find("fresh") == nullptr);
  CHECK_EQ(q(db, "SELECT COUNT(*) FROM t"), std::string("2"));
  CHECK(db.catalog().find("t")->findIndex("t_v") == nullptr);
  db.query("CREATE INDEX t_v ON t (v)");
}

TEST(a_failed_statement_keeps_the_transaction_open) {
  auto db = seeded();
  db.execute("BEGIN; INSERT INTO t VALUES (3, 30);");
  CHECK_ERROR(db.query("INSERT INTO t VALUES (4, 40), (1, 0)"), Constraint);
  CHECK(db.inTransaction());
  CHECK_EQ(q(db, "SELECT id FROM t ORDER BY id"), std::string("1;2;3"));
  db.query("COMMIT");
  CHECK_EQ(q(db, "SELECT id FROM t ORDER BY id"), std::string("1;2;3"));
}

TEST(transaction_statements_out_of_place) {
  auto db = seeded();
  CHECK_ERROR(db.query("COMMIT"), Transaction);
  CHECK_ERROR(db.query("ROLLBACK"), Transaction);
  db.query("BEGIN");
  CHECK_ERROR(db.query("BEGIN"), Transaction);
  CHECK(db.inTransaction());
  db.query("ROLLBACK");
}

TEST(snapshot_is_not_touched_by_writes_to_other_tables) {
  Database db;
  db.execute("CREATE TABLE a (x INT); CREATE TABLE b (x INT); INSERT INTO a VALUES (1);");
  db.execute("BEGIN; INSERT INTO b VALUES (2); INSERT INTO a VALUES (3); ROLLBACK;");
  CHECK_EQ(q(db, "SELECT x FROM a"), std::string("1"));
  CHECK_EQ(q(db, "SELECT COUNT(*) FROM b"), std::string("0"));
}

TEST(transactions_can_follow_each_other) {
  auto db = seeded();
  db.execute("BEGIN; INSERT INTO t VALUES (3, 30); COMMIT; BEGIN; DELETE FROM t; ROLLBACK;");
  CHECK_EQ(q(db, "SELECT COUNT(*) FROM t"), std::string("3"));
  db.execute("BEGIN; DELETE FROM t; COMMIT;");
  CHECK_EQ(q(db, "SELECT COUNT(*) FROM t"), std::string("0"));
}

TEST(begin_transaction_and_keyword_variants) {
  auto db = seeded();
  db.execute("BEGIN TRANSACTION; DELETE FROM t; ROLLBACK TRANSACTION;");
  CHECK_EQ(q(db, "SELECT COUNT(*) FROM t"), std::string("2"));
  db.execute("BEGIN TRANSACTION; DELETE FROM t; COMMIT TRANSACTION;");
  CHECK_EQ(q(db, "SELECT COUNT(*) FROM t"), std::string("0"));
}

TEST(recreating_a_dropped_table_inside_a_transaction) {
  auto db = seeded();
  db.execute("BEGIN; DROP TABLE t; CREATE TABLE t (other TEXT); INSERT INTO t VALUES ('x');");
  CHECK_EQ(q(db, "SELECT other FROM t"), std::string("x"));
  db.query("ROLLBACK");
  CHECK_EQ(q(db, "SELECT id FROM t ORDER BY id"), std::string("1;2"));
}

TEST_MAIN
