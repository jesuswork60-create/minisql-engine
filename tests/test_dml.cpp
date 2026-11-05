#include "test_util.h"

using namespace minisql;
using testutil::rowsText;

static std::string q(Database& db, const std::string& sql) { return rowsText(db.query(sql)); }

TEST(create_and_drop) {
  Database db;
  CHECK_EQ(db.query("CREATE TABLE t (a INT)").status, std::string("CREATE TABLE"));
  CHECK_ERROR(db.query("CREATE TABLE t (a INT)"), Catalog);
  CHECK_ERROR(db.query("CREATE TABLE T (a INT)"), Catalog);
  db.query("CREATE TABLE IF NOT EXISTS t (b INT)");
  CHECK(db.catalog().find("t")->columnIndex("a").has_value());
  db.query("DROP TABLE t");
  CHECK_ERROR(db.query("DROP TABLE t"), Catalog);
  db.query("DROP TABLE IF EXISTS t");
}

TEST(bad_table_definitions) {
  Database db;
  CHECK_ERROR(db.query("CREATE TABLE t (a INT, A TEXT)"), Catalog);
  CHECK_ERROR(db.query("CREATE TABLE t (a INT PRIMARY KEY, b INT PRIMARY KEY)"), Catalog);
  CHECK_ERROR(db.query("CREATE TABLE t (a BLOB)"), Syntax);
  CHECK_ERROR(db.query("CREATE TABLE t (a INT DEFAULT 'x')"), Type);
  CHECK_ERROR(db.query("CREATE TABLE t (a INT NOT NULL DEFAULT NULL)"), Constraint);
  CHECK_ERROR(db.query("CREATE TABLE t (a INT DEFAULT b)"), Syntax);
  CHECK(db.catalog().find("t") == nullptr);
}

TEST(insert_forms) {
  Database db;
  db.execute("CREATE TABLE t (a INT, b TEXT DEFAULT 'def', c FLOAT);");
  CHECK_EQ(db.query("INSERT INTO t VALUES (1, 'x', 1.5), (2, 'y', 2.5)").affected, size_t{2});
  db.query("INSERT INTO t (c, a) VALUES (3, 3)");
  db.query("INSERT INTO t (a) VALUES (1 + 3)");
  CHECK_EQ(q(db, "SELECT a, b, c FROM t WHERE a >= 3"), std::string("3|def|3.0;4|def|NULL"));
}

TEST(insert_mistakes) {
  Database db;
  db.execute("CREATE TABLE t (a INT, b TEXT);");
  CHECK_ERROR(db.query("INSERT INTO t VALUES (1)"), Syntax);
  CHECK_ERROR(db.query("INSERT INTO t (a) VALUES (1, 2)"), Syntax);
  CHECK_ERROR(db.query("INSERT INTO t (a, a) VALUES (1, 2)"), Catalog);
  CHECK_ERROR(db.query("INSERT INTO t (nope) VALUES (1)"), Catalog);
  CHECK_ERROR(db.query("INSERT INTO nope VALUES (1)"), Catalog);
  CHECK_ERROR(db.query("INSERT INTO t VALUES ('x', 'y')"), Type);
  CHECK_ERROR(db.query("INSERT INTO t VALUES (1, 2)"), Type);
  CHECK_EQ(q(db, "SELECT COUNT(*) FROM t"), std::string("0"));
}

TEST(int_goes_into_float_column_but_not_the_reverse) {
  Database db;
  db.execute("CREATE TABLE t (f FLOAT, i INT);");
  db.query("INSERT INTO t VALUES (1, 2)");
  CHECK_EQ(q(db, "SELECT f, i FROM t"), std::string("1.0|2"));
  CHECK_ERROR(db.query("INSERT INTO t VALUES (1, 2.5)"), Type);
}

TEST(not_null_constraint) {
  Database db;
  db.execute("CREATE TABLE t (a INT NOT NULL, b INT);");
  CHECK_ERROR(db.query("INSERT INTO t VALUES (NULL, 1)"), Constraint);
  CHECK_ERROR(db.query("INSERT INTO t (b) VALUES (1)"), Constraint);
  db.query("INSERT INTO t VALUES (1, NULL)");
  CHECK_ERROR(db.query("UPDATE t SET a = NULL"), Constraint);
  CHECK_EQ(q(db, "SELECT a FROM t"), std::string("1"));
}

TEST(primary_key_rejects_duplicates_and_null) {
  Database db;
  db.execute("CREATE TABLE t (id INT PRIMARY KEY, v TEXT);");
  db.query("INSERT INTO t VALUES (1, 'a')");
  CHECK_ERROR(db.query("INSERT INTO t VALUES (1, 'b')"), Constraint);
  CHECK_ERROR(db.query("INSERT INTO t VALUES (NULL, 'b')"), Constraint);
  CHECK_EQ(q(db, "SELECT v FROM t"), std::string("a"));
}

TEST(multi_row_insert_is_all_or_nothing) {
  Database db;
  db.execute("CREATE TABLE t (id INT PRIMARY KEY, v INT NOT NULL);");
  db.query("INSERT INTO t VALUES (1, 1)");
  CHECK_ERROR(db.query("INSERT INTO t VALUES (2, 2), (3, 3), (1, 4)"), Constraint);
  CHECK_ERROR(db.query("INSERT INTO t VALUES (2, 2), (2, 3)"), Constraint);
  CHECK_ERROR(db.query("INSERT INTO t VALUES (2, 2), (3, NULL)"), Constraint);
  CHECK_ERROR(db.query("INSERT INTO t VALUES (2, 2), (3, 1 / 0)"), Arithmetic);
  CHECK_EQ(q(db, "SELECT id FROM t"), std::string("1"));
  CHECK_EQ(q(db, "SELECT id FROM t WHERE id = 2"), std::string(""));
  db.query("INSERT INTO t VALUES (2, 2)");
  CHECK_EQ(q(db, "SELECT id FROM t ORDER BY id"), std::string("1;2"));
}

TEST(update_basics) {
  Database db;
  db.execute("CREATE TABLE t (id INT PRIMARY KEY, v INT); INSERT INTO t VALUES (1, 10), (2, 20), (3, 30);");
  CHECK_EQ(db.query("UPDATE t SET v = v + 1 WHERE id > 1").affected, size_t{2});
  CHECK_EQ(q(db, "SELECT v FROM t ORDER BY id"), std::string("10;21;31"));
  CHECK_EQ(db.query("UPDATE t SET v = 0 WHERE id = 99").affected, size_t{0});
  CHECK_EQ(db.query("UPDATE t SET v = 0").affected, size_t{3});
}

TEST(update_reads_the_old_row) {
  Database db;
  db.execute("CREATE TABLE t (a INT, b INT); INSERT INTO t VALUES (1, 2);");
  db.query("UPDATE t SET a = b, b = a");
  CHECK_EQ(q(db, "SELECT a, b FROM t"), std::string("2|1"));
}

TEST(primary_key_shift_touching_every_row_succeeds) {
  Database db;
  db.execute("CREATE TABLE t (id INT PRIMARY KEY); INSERT INTO t VALUES (1), (2), (3);");
  db.query("UPDATE t SET id = id + 1");
  CHECK_EQ(q(db, "SELECT id FROM t ORDER BY id"), std::string("2;3;4"));
  CHECK_EQ(q(db, "SELECT id FROM t WHERE id = 1"), std::string(""));
  CHECK_EQ(q(db, "SELECT id FROM t WHERE id = 4"), std::string("4"));
}

TEST(primary_key_swap_succeeds) {
  Database db;
  db.execute("CREATE TABLE t (id INT PRIMARY KEY, v TEXT); INSERT INTO t VALUES (1, 'a'), (2, 'b');");
  db.query("UPDATE t SET id = 3 - id");
  CHECK_EQ(q(db, "SELECT v FROM t WHERE id = 1"), std::string("b"));
  CHECK_EQ(q(db, "SELECT v FROM t WHERE id = 2"), std::string("a"));
}

TEST(update_that_collides_changes_nothing) {
  Database db;
  db.execute("CREATE TABLE t (id INT PRIMARY KEY, v INT); INSERT INTO t VALUES (1, 1), (2, 2), (3, 3);");
  CHECK_ERROR(db.query("UPDATE t SET id = 1"), Constraint);
  CHECK_ERROR(db.query("UPDATE t SET id = 3 WHERE id = 1"), Constraint);
  CHECK_ERROR(db.query("UPDATE t SET v = 100 / (v - 2)"), Arithmetic);
  CHECK_EQ(q(db, "SELECT id, v FROM t ORDER BY id"), std::string("1|1;2|2;3|3"));
  CHECK_EQ(q(db, "SELECT v FROM t WHERE id = 3"), std::string("3"));
}

TEST(update_mistakes) {
  Database db;
  db.execute("CREATE TABLE t (a INT, b TEXT); INSERT INTO t VALUES (1, 'x');");
  CHECK_ERROR(db.query("UPDATE t SET nope = 1"), Catalog);
  CHECK_ERROR(db.query("UPDATE t SET a = 1, a = 2"), Catalog);
  CHECK_ERROR(db.query("UPDATE t SET a = 'x'"), Type);
  CHECK_ERROR(db.query("UPDATE t SET a = nope"), Catalog);
  CHECK_ERROR(db.query("UPDATE nope SET a = 1"), Catalog);
  CHECK_ERROR(db.query("UPDATE t SET a = 2 WHERE nope = 1"), Catalog);
  CHECK_ERROR(db.query("UPDATE t SET a = 2 WHERE a"), Type);
}

TEST(delete_with_where_and_all) {
  Database db;
  db.execute("CREATE TABLE t (id INT PRIMARY KEY); INSERT INTO t VALUES (1), (2), (3), (4);");
  CHECK_EQ(db.query("DELETE FROM t WHERE id % 2 = 0").affected, size_t{2});
  CHECK_EQ(q(db, "SELECT id FROM t"), std::string("1;3"));
  CHECK_EQ(q(db, "SELECT id FROM t WHERE id = 2"), std::string(""));
  db.query("INSERT INTO t VALUES (2)");
  CHECK_EQ(db.query("DELETE FROM t").affected, size_t{3});
  CHECK_EQ(q(db, "SELECT COUNT(*) FROM t"), std::string("0"));
  CHECK_ERROR(db.query("DELETE FROM nope"), Catalog);
}

TEST(delete_with_failing_where_removes_nothing) {
  Database db;
  db.execute("CREATE TABLE t (id INT); INSERT INTO t VALUES (1), (0), (2);");
  CHECK_ERROR(db.query("DELETE FROM t WHERE 1 / id = 1"), Arithmetic);
  CHECK_EQ(q(db, "SELECT COUNT(*) FROM t"), std::string("3"));
}

TEST(delete_null_where_keeps_the_row) {
  Database db;
  db.execute("CREATE TABLE t (a INT); INSERT INTO t VALUES (NULL), (1);");
  db.query("DELETE FROM t WHERE a = 1");
  CHECK_EQ(q(db, "SELECT a FROM t"), std::string("NULL"));
}

TEST(heavy_churn_keeps_rows_and_indexes_right) {
  // Enough deletes to trigger storage compaction more than once.
  Database db;
  db.execute("CREATE TABLE t (id INT PRIMARY KEY, v INT); CREATE INDEX t_v ON t (v);");
  for (int round = 0; round < 5; ++round) {
    for (int i = 0; i < 200; ++i) {
      db.query("INSERT INTO t VALUES (" + std::to_string(round * 1000 + i) + ", " + std::to_string(i % 7) + ")");
    }
    db.query("DELETE FROM t WHERE id % 2 = 0");
  }
  CHECK_EQ(q(db, "SELECT COUNT(*) FROM t"), std::string("500"));
  CHECK_EQ(q(db, "SELECT v FROM t WHERE id = 4001"), std::string("1"));
  CHECK_EQ(q(db, "SELECT COUNT(*) FROM t WHERE id = 4000"), std::string("0"));
  // Index results agree with a scan that cannot use the index.
  CHECK_EQ(q(db, "SELECT COUNT(*) FROM t WHERE v = 3"), q(db, "SELECT COUNT(*) FROM t WHERE v + 0 = 3"));
  CHECK_EQ(q(db, "SELECT id FROM t WHERE v = 3 ORDER BY id"), q(db, "SELECT id FROM t WHERE v + 0 = 3 ORDER BY id"));
}

TEST(script_stops_at_the_failure_but_keeps_earlier_work) {
  Database db;
  CHECK_ERROR(db.execute("CREATE TABLE t (a INT); INSERT INTO t VALUES (1); INSERT INTO t VALUES ('x'); "
                         "INSERT INTO t VALUES (3);"),
              Type);
  CHECK_EQ(q(db, "SELECT a FROM t"), std::string("1"));
}

TEST(syntax_error_in_a_script_runs_nothing) {
  Database db;
  CHECK_ERROR(db.execute("CREATE TABLE t (a INT); SELEC 1;"), Syntax);
  CHECK(db.catalog().find("t") == nullptr);
}

TEST(dump_round_trip) {
  Database db;
  db.execute(
      "CREATE TABLE \"odd name\" (id INT PRIMARY KEY, t TEXT NOT NULL DEFAULT 'it''s', f FLOAT, b BOOL DEFAULT TRUE);"
      "CREATE UNIQUE INDEX u_t ON \"odd name\" (t);"
      "CREATE INDEX i_f ON \"odd name\" (f);"
      "INSERT INTO \"odd name\" VALUES (1, 'a''b', 1.5, TRUE), (2, 'line\nbreak', NULL, NULL), (3, '', 0.1, FALSE),"
      " (-9223372036854775808, 'min', 1e300, TRUE);"
      "CREATE TABLE other (x INT);"
      "INSERT INTO other VALUES (1);"
      "DELETE FROM other;");
  std::ostringstream first;
  db.dump(first);

  Database copy;
  copy.execute(first.str());
  std::ostringstream second;
  copy.dump(second);
  CHECK_EQ(second.str(), first.str());

  std::string all = "SELECT * FROM \"odd name\" ORDER BY id";
  CHECK_EQ(rowsText(copy.query(all)), rowsText(db.query(all)));
  CHECK_ERROR(copy.query("INSERT INTO \"odd name\" VALUES (9, 'min', 0.0, TRUE)"), Constraint);
  CHECK_EQ(rowsText(copy.query("SELECT COUNT(*) FROM other")), std::string("0"));
  CHECK(testutil::contains(testutil::planText(copy, "SELECT id FROM \"odd name\" WHERE f = 1.5"), "i_f"));
}

TEST_MAIN
