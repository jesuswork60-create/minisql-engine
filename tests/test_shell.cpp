#include "minisql/shell.h"

#include <cstdio>
#include <fstream>
#include <sstream>

#include "test_util.h"

using namespace minisql;
using testutil::contains;

namespace {

struct Session {
  Database db;
  std::ostringstream out;
  std::ostringstream err;
  ShellOptions options;

  int run(const std::string& text, const std::string& source = "") {
    Shell shell(db, out, err, options);
    std::istringstream in(text);
    return shell.runStream(in, source);
  }
};

// A file beside the running test, removed when the test ends.
struct TempFile {
  std::string path;
  explicit TempFile(std::string name, const std::string& content = "") : path(std::move(name)) {
    std::ofstream(path) << content;
  }
  ~TempFile() { std::remove(path.c_str()); }
};

std::string readAll(const std::string& path) {
  std::ifstream in(path);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

}  // namespace

TEST(statement_across_lines) {
  Session s;
  CHECK_EQ(s.run("SELECT\n  1 + 1\n  AS two\n;\n"), 0);
  CHECK(contains(s.out.str(), "| two |"));
  CHECK(contains(s.out.str(), "|   2 |"));
}

TEST(several_statements_on_a_line) {
  Session s;
  CHECK_EQ(s.run("CREATE TABLE t (a INT); INSERT INTO t VALUES (1); SELECT a FROM t;\n"), 0);
  CHECK(contains(s.out.str(), "CREATE TABLE\nINSERT 1\n"));
  CHECK(contains(s.out.str(), "(1 row)"));
}

TEST(semicolon_inside_a_string_does_not_end_the_statement) {
  Session s;
  CHECK_EQ(s.run("SELECT 'a;\nb' AS v;\n"), 0);
  CHECK(contains(s.out.str(), "a;\\nb"));
}

TEST(comment_only_lines_are_ignored) {
  Session s;
  CHECK_EQ(s.run("-- a note\n\n/* another */\nSELECT 1;\n"), 0);
  CHECK(s.err.str().empty());
  CHECK(contains(s.out.str(), "(1 row)"));
}

TEST(comment_after_the_semicolon_still_ends_the_statement) {
  Session s;
  CHECK_EQ(s.run("SELECT 1; -- done\nSELECT 2;\n"), 0);
  CHECK(s.err.str().empty());
}

TEST(last_statement_may_omit_the_semicolon) {
  Session s;
  CHECK_EQ(s.run("SELECT 7 AS v"), 0);
  CHECK(contains(s.out.str(), "| 7 |"));
}

TEST(unfinished_statement_at_end_of_input_reports_an_error) {
  Session s;
  CHECK_EQ(s.run("SELECT 1 +"), 1);
  CHECK(contains(s.err.str(), "syntax error"));
}

TEST(errors_continue_by_default_and_set_the_exit_status) {
  Session s;
  CHECK_EQ(s.run("SELECT nope;\nSELECT 5 AS v;\n"), 1);
  CHECK(contains(s.err.str(), "no such column"));
  CHECK(contains(s.out.str(), "| 5 |"));
}

TEST(stop_on_error_stops) {
  Session s;
  s.options.stopOnError = true;
  CHECK_EQ(s.run("SELECT nope;\nSELECT 5 AS v;\n"), 1);
  CHECK(!contains(s.out.str(), "| 5 |"));
}

TEST(script_errors_name_the_file_and_real_line) {
  Session s;
  CHECK_EQ(s.run("SELECT 1;\n\nSELECT\n  nope;\n", "demo.sql"), 1);
  CHECK(contains(s.err.str(), "demo.sql: "));
  CHECK(contains(s.err.str(), "line 4, column 3"));
  CHECK(contains(s.err.str(), "  nope;"));
}

TEST(earlier_output_survives_a_later_failure_on_one_line) {
  Session s;
  CHECK_EQ(s.run("SELECT 1 AS first_one; SELECT nope;\n"), 1);
  CHECK(contains(s.out.str(), "first_one"));
}

TEST(prompts_show_only_when_asked) {
  Session quiet;
  quiet.run("SELECT 1;\n");
  CHECK(!contains(quiet.out.str(), "minisql>"));
  Session interactive;
  interactive.options.prompts = true;
  interactive.run("SELECT\n1;\n");
  CHECK(contains(interactive.out.str(), "minisql> "));
  CHECK(contains(interactive.out.str(), "   ...> "));
}

TEST(tables_and_schema_commands) {
  Session s;
  s.run("CREATE TABLE b (id INT PRIMARY KEY, n TEXT NOT NULL DEFAULT 'x');\n"
        "CREATE INDEX b_n ON b (n);\n"
        "CREATE TABLE a (v FLOAT);\n");
  s.out.str("");
  CHECK_EQ(s.run(".tables\n"), 0);
  CHECK_EQ(s.out.str(), std::string("a\nb\n"));
  s.out.str("");
  CHECK_EQ(s.run(".schema b\n"), 0);
  CHECK_EQ(s.out.str(), std::string("CREATE TABLE b (id INT PRIMARY KEY, n TEXT NOT NULL DEFAULT 'x');\n"
                                    "CREATE INDEX b_n ON b (n);\n"));
  s.out.str("");
  CHECK_EQ(s.run(".schema\n"), 0);
  CHECK(contains(s.out.str(), "CREATE TABLE a"));
  CHECK_EQ(s.run(".schema nope\n"), 1);
}

TEST(dump_command_output_rebuilds_the_database) {
  Session s;
  s.run("CREATE TABLE t (a INT, b TEXT); INSERT INTO t VALUES (1, 'it''s');\n");
  s.out.str("");
  CHECK_EQ(s.run(".dump\n"), 0);
  std::string script = s.out.str();
  CHECK(contains(script, "BEGIN;\n"));
  CHECK(contains(script, "INSERT INTO t VALUES (1, 'it''s');"));

  Session copy;
  CHECK_EQ(copy.run(script), 0);
  CHECK_EQ(testutil::rowsText(copy.db.query("SELECT a, b FROM t")), std::string("1|it's"));
}

TEST(dump_to_file) {
  TempFile file("shell_test_dump.sql");
  Session s;
  s.run("CREATE TABLE t (a INT);\n");
  CHECK_EQ(s.run(".dump " + file.path + "\n"), 0);
  CHECK(contains(readAll(file.path), "CREATE TABLE t"));
  CHECK_EQ(s.run(".dump no/such/dir/x.sql\n"), 1);
}

TEST(import_and_export_commands) {
  TempFile input("shell_test_input.csv", "id,name\n1,Ann\n2,\"Bo, Jr\"\n");
  TempFile output("shell_test_output.csv");
  Session s;
  CHECK_EQ(s.run(".import " + input.path + " people\n"), 0);
  CHECK(contains(s.out.str(), "IMPORT 2"));
  CHECK_EQ(testutil::rowsText(s.db.query("SELECT id + 1, name FROM people")), std::string("2|Ann;3|Bo, Jr"));
  CHECK_EQ(s.run(".export people " + output.path + "\n"), 0);
  CHECK_EQ(readAll(output.path), readAll(input.path));
}

TEST(import_and_export_failures) {
  TempFile bad("shell_test_bad.csv", "a\n\"open\n");
  Session s;
  CHECK_EQ(s.run(".import nope.csv t\n"), 1);
  CHECK(contains(s.err.str(), "cannot read nope.csv"));
  CHECK_EQ(s.run(".import " + bad.path + " t\n"), 1);
  CHECK(contains(s.err.str(), "unterminated"));
  CHECK_EQ(s.run(".export nope shell_test_never.csv\n"), 1);
  std::remove("shell_test_never.csv");
  CHECK_EQ(s.run(".import onlyonearg\n"), 1);
  CHECK(contains(s.err.str(), "usage: .import"));
}

TEST(file_names_with_spaces_can_be_quoted) {
  TempFile input("shell test input.csv", "a\n1\n");
  Session s;
  CHECK_EQ(s.run(".import \"" + input.path + "\" t\n"), 0);
  CHECK_EQ(testutil::rowsText(s.db.query("SELECT a FROM t")), std::string("1"));
}

TEST(read_command_runs_a_script) {
  TempFile script("shell_test_read.sql", "CREATE TABLE t (a INT);\nINSERT INTO t VALUES (9);\nSELECT nope;\n");
  Session s;
  CHECK_EQ(s.run(".read " + script.path + "\n"), 1);
  CHECK(contains(s.err.str(), script.path + ": "));
  CHECK(contains(s.err.str(), "line 3"));
  CHECK_EQ(testutil::rowsText(s.db.query("SELECT a FROM t")), std::string("9"));
}

TEST(read_command_that_includes_itself_stops) {
  TempFile script("shell_test_loop.sql", ".read shell_test_loop.sql\n");
  Session s;
  CHECK_EQ(s.run(".read " + script.path + "\n"), 1);
  CHECK(contains(s.err.str(), "nested too deeply"));
}

TEST(quit_stops_reading) {
  Session s;
  CHECK_EQ(s.run("SELECT 1;\n.quit\nSELECT nope;\n"), 0);
  CHECK(s.err.str().empty());
}

TEST(quit_inside_a_read_script_ends_the_session) {
  TempFile script("shell_test_quit.sql", ".quit\n");
  Session s;
  CHECK_EQ(s.run(".read " + script.path + "\nSELECT nope;\n"), 0);
  CHECK(s.err.str().empty());
}

TEST(dot_commands_are_only_recognised_between_statements) {
  Session s;
  // The second line belongs to the open statement, so this is a syntax error,
  // not a request to list tables.
  CHECK_EQ(s.run("SELECT 1 +\n.tables\n;\n"), 1);
  CHECK(contains(s.err.str(), "syntax error"));
}

TEST(unknown_commands_and_help) {
  Session s;
  CHECK_EQ(s.run(".frobnicate\n"), 1);
  CHECK(contains(s.err.str(), "unknown command .frobnicate"));
  CHECK_EQ(s.run(".help\n"), 0);
  CHECK(contains(s.out.str(), ".import FILE TABLE"));
}

TEST(transaction_state_spans_commands) {
  Session s;
  CHECK_EQ(s.run("CREATE TABLE t (a INT);\nBEGIN;\nINSERT INTO t VALUES (1);\nROLLBACK;\nSELECT COUNT(*) AS n FROM t;\n"), 0);
  CHECK(contains(s.out.str(), "| 0 |"));
}

TEST(crlf_input_works) {
  Session s;
  CHECK_EQ(s.run("SELECT\r\n 4 AS v;\r\n.tables\r\n"), 0);
  CHECK(contains(s.out.str(), "| 4 |"));
  CHECK(s.err.str().empty());
}

TEST_MAIN
