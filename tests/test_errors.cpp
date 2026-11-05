#include "minisql/parser.h"
#include "test_util.h"

using namespace minisql;

static Error capture(Database& db, const std::string& sql) {
  try {
    db.execute(sql);
  } catch (const Error& e) {
    return e;
  }
  testutil::reportFailure(__FILE__, __LINE__, "expected an error from: " + sql);
  return Error(ErrorKind::Syntax, "none");
}

static Database seeded() {
  Database db;
  db.execute("CREATE TABLE t (id INT PRIMARY KEY, name TEXT NOT NULL); INSERT INTO t VALUES (1, 'a');");
  return db;
}

#define CHECK_POS(error, expectedLine, expectedColumn)  \
  do {                                                  \
    const Error caughtError = (error);                  \
    CHECK_EQ(caughtError.pos().line, expectedLine);     \
    CHECK_EQ(caughtError.pos().column, expectedColumn); \
  } while (false)

TEST(unknown_table_position) {
  auto db = seeded();
  CHECK_POS(capture(db, "SELECT 1\nFROM nowhere"), 2, 6);
}

TEST(unknown_column_position) {
  auto db = seeded();
  CHECK_POS(capture(db, "SELECT id,\n       nope FROM t"), 2, 8);
  CHECK_POS(capture(db, "SELECT id FROM t WHERE\n  nope = 1"), 2, 3);
}

TEST(runtime_arithmetic_error_position) {
  auto db = seeded();
  CHECK_POS(capture(db, "SELECT 1 +\n  id / 0 FROM t"), 2, 6);
}

TEST(type_error_in_where_position) {
  auto db = seeded();
  CHECK_POS(capture(db, "SELECT id FROM t WHERE name = 1"), 1, 29);
}

TEST(type_error_in_an_index_lookup_position) {
  auto db = seeded();
  CHECK_POS(capture(db, "SELECT id FROM t WHERE id = 'x'"), 1, 27);
  CHECK_POS(capture(db, "SELECT id FROM t WHERE\n  'x' = id"), 2, 7);
}

TEST(insert_value_error_positions) {
  auto db = seeded();
  CHECK_POS(capture(db, "INSERT INTO t VALUES\n (2, 'b'),\n (3, 4)"), 3, 6);
  CHECK_POS(capture(db, "INSERT INTO t VALUES\n (2, 'b'),\n (3)"), 3, 2);
  CHECK_POS(capture(db, "INSERT INTO t VALUES (2, NULL)"), 1, 26);
}

TEST(position_in_a_later_statement_of_a_script) {
  auto db = seeded();
  CHECK_POS(capture(db, "SELECT 1;\nSELECT 2;\nSELECT nope FROM t;"), 3, 8);
}

TEST(statement_level_errors_fall_back_to_the_statement_position) {
  auto db = seeded();
  CHECK_POS(capture(db, "SELECT 1;\n  COMMIT;"), 2, 3);
  CHECK_POS(capture(db, "\n\nCREATE TABLE t (a INT)"), 3, 1);
  CHECK_POS(capture(db, "\nINSERT INTO t VALUES (1, 'dup')"), 2, 1);
}

TEST(message_format) {
  auto db = seeded();
  std::string text = capture(db, "SELECT nope FROM t").what();
  CHECK_EQ(text, std::string("catalog error at line 1, column 8: no such column: nope"));
  text = capture(db, "SELEC").what();
  CHECK(testutil::contains(text, "syntax error at line 1, column 1"));
}

TEST(render_shows_the_line_and_a_caret) {
  auto db = seeded();
  std::string sql = "SELECT id\nFROM nowhere";
  std::string rendered = renderError(capture(db, sql), sql);
  CHECK(testutil::contains(rendered, "line 2, column 6"));
  CHECK(testutil::contains(rendered, "  FROM nowhere\n"));
  CHECK(testutil::contains(rendered, "\n       ^"));
}

TEST(render_with_line_offset_reports_file_lines_but_shows_the_right_text) {
  auto db = seeded();
  std::string sql = "SELECT id\nFROM nowhere";
  std::string rendered = renderError(capture(db, sql), sql, 10);
  CHECK(testutil::contains(rendered, "line 12, column 6"));
  CHECK(testutil::contains(rendered, "FROM nowhere"));
}

TEST(render_survives_a_position_past_the_text) {
  Error error(ErrorKind::Syntax, "boom", SourcePos{99, 3});
  CHECK(testutil::contains(renderError(error, "short"), "boom"));
}

TEST(failed_statements_change_nothing) {
  auto db = seeded();
  std::ostringstream before;
  db.dump(before);
  for (const char* bad : {
           "INSERT INTO t VALUES (2, 'b'), (1, 'dup')",
           "INSERT INTO t VALUES (2, 'b'), (3, NULL)",
           "UPDATE t SET name = NULL",
           "UPDATE t SET id = 1 / 0",
           "DELETE FROM t WHERE 1 / 0 = 1",
           "CREATE UNIQUE INDEX bad ON t (nope)",
           "CREATE TABLE t (a INT)",
           "DROP TABLE nope",
       }) {
    bool failed = false;
    try {
      db.query(bad);
    } catch (const Error&) {
      failed = true;
    }
    CHECK(failed);
  }
  std::ostringstream after;
  db.dump(after);
  CHECK_EQ(after.str(), before.str());
}

TEST_MAIN
