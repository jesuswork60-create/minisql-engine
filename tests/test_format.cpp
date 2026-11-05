#include "minisql/format.h"

#include <sstream>

#include "test_util.h"

using namespace minisql;

static std::string render(Database& db, const std::string& sql) {
  std::ostringstream out;
  printResult(db.query(sql), out);
  return out.str();
}

TEST(table_layout_and_alignment) {
  Database db;
  db.execute("CREATE TABLE t (id INT, name TEXT, f FLOAT); INSERT INTO t VALUES (1, 'Ann', 1.5), (20, 'Bo', NULL);");
  CHECK_EQ(render(db, "SELECT * FROM t"),
           std::string("+----+------+------+\n"
                       "| id | name | f    |\n"
                       "+----+------+------+\n"
                       "|  1 | Ann  |  1.5 |\n"
                       "| 20 | Bo   | NULL |\n"
                       "+----+------+------+\n"
                       "(2 rows)\n"));
}

TEST(singular_row_count) {
  Database db;
  CHECK(testutil::contains(render(db, "SELECT 1"), "(1 row)\n"));
}

TEST(empty_result_still_shows_the_header) {
  Database db;
  db.execute("CREATE TABLE t (abc INT);");
  CHECK_EQ(render(db, "SELECT abc FROM t"),
           std::string("+-----+\n"
                       "| abc |\n"
                       "+-----+\n"
                       "+-----+\n"
                       "(0 rows)\n"));
}

TEST(non_query_prints_its_status) {
  Database db;
  CHECK_EQ(render(db, "CREATE TABLE t (a INT)"), std::string("CREATE TABLE\n"));
  CHECK_EQ(render(db, "INSERT INTO t VALUES (1), (2)"), std::string("INSERT 2\n"));
  CHECK_EQ(render(db, "UPDATE t SET a = 3"), std::string("UPDATE 2\n"));
  CHECK_EQ(render(db, "DELETE FROM t"), std::string("DELETE 2\n"));
  CHECK_EQ(render(db, "BEGIN"), std::string("BEGIN\n"));
}

TEST(multibyte_text_keeps_the_grid_straight) {
  Database db;
  std::string out = render(db, "SELECT 'caf\xC3\xA9' AS word");
  CHECK_EQ(out, std::string("+------+\n"
                            "| word |\n"
                            "+------+\n"
                            "| caf\xC3\xA9 |\n"
                            "+------+\n"
                            "(1 row)\n"));
}

TEST(line_breaks_are_shown_escaped) {
  Database db;
  std::string out = render(db, "SELECT 'a\nb' AS v");
  CHECK(testutil::contains(out, "| a\\nb |"));
}

TEST_MAIN
