#include "minisql/csv.h"

#include <sstream>

#include "test_util.h"

using namespace minisql;
using testutil::rowsText;

static std::string q(Database& db, const std::string& sql) { return rowsText(db.query(sql)); }

static size_t load(Database& db, const std::string& table, const std::string& text, CsvOptions options = {}) {
  std::istringstream in(text);
  return db.importCsv(table, in, options);
}

static std::vector<std::vector<std::string>> records(const std::string& text) {
  std::istringstream in(text);
  CsvReader reader(in, ',');
  std::vector<std::vector<std::string>> out;
  std::vector<CsvField> fields;
  while (reader.readRecord(fields)) {
    std::vector<std::string> record;
    for (const CsvField& field : fields) record.push_back(field.text);
    out.push_back(std::move(record));
  }
  return out;
}

TEST(reader_handles_quotes_and_newlines) {
  auto rows = records("a,\"b,c\",\"d\"\"e\"\n\"multi\nline\",x,y\n");
  CHECK_EQ(rows.size(), size_t{2});
  CHECK_EQ(rows[0][1], std::string("b,c"));
  CHECK_EQ(rows[0][2], std::string("d\"e"));
  CHECK_EQ(rows[1][0], std::string("multi\nline"));
}

TEST(reader_handles_crlf_and_missing_final_newline) {
  auto rows = records("a,b\r\nc,d");
  CHECK_EQ(rows.size(), size_t{2});
  CHECK_EQ(rows[0][1], std::string("b"));
  CHECK_EQ(rows[1][1], std::string("d"));
}

TEST(reader_keeps_crlf_inside_quotes) {
  auto rows = records("\"a\r\nb\",c\r\n");
  CHECK_EQ(rows[0][0], std::string("a\r\nb"));
  CHECK_EQ(rows[0][1], std::string("c"));
}

TEST(reader_skips_blank_lines) {
  auto rows = records("\n\na\n\n\nb\n\n");
  CHECK_EQ(rows.size(), size_t{2});
  CHECK_EQ(rows[1][0], std::string("b"));
}

TEST(reader_keeps_trailing_empty_field) {
  auto rows = records("a,\nb,\"\"\n,");
  CHECK_EQ(rows.size(), size_t{3});
  CHECK_EQ(rows[0].size(), size_t{2});
  CHECK_EQ(rows[1].size(), size_t{2});
  CHECK_EQ(rows[2].size(), size_t{2});
}

TEST(reader_strips_byte_order_mark) {
  auto rows = records("\xEF\xBB\xBF" "a,b\n");
  CHECK_EQ(rows[0][0], std::string("a"));
}

TEST(reader_reports_bad_quoting_with_a_line) {
  try {
    records("ok\n\"never closed\n");
    CHECK(false);
  } catch (const Error& e) {
    CHECK(e.kind() == ErrorKind::Io);
    CHECK_EQ(e.pos().line, 2);
  }
  CHECK_ERROR(records("\"a\"b,c\n"), Io);
}

TEST(reader_tracks_record_lines_past_embedded_newlines) {
  std::istringstream in("\"a\nb\"\nc\n");
  CsvReader reader(in, ',');
  std::vector<CsvField> fields;
  CHECK(reader.readRecord(fields));
  CHECK_EQ(reader.recordLine(), 1);
  CHECK(reader.readRecord(fields));
  CHECK_EQ(reader.recordLine(), 3);
}

TEST(import_into_existing_table_by_header) {
  Database db;
  db.execute("CREATE TABLE t (id INT PRIMARY KEY, name TEXT DEFAULT 'anon', score FLOAT);");
  CHECK_EQ(load(db, "t", "score,id\n1.5,1\n2,2\n"), size_t{2});
  CHECK_EQ(q(db, "SELECT id, name, score FROM t ORDER BY id"), std::string("1|anon|1.5;2|anon|2.0"));
}

TEST(import_without_header_is_positional) {
  Database db;
  db.execute("CREATE TABLE t (a INT, b TEXT);");
  CsvOptions options;
  options.header = false;
  CHECK_EQ(load(db, "t", "1,x\n2,y\n", options), size_t{2});
  CHECK_EQ(q(db, "SELECT a, b FROM t"), std::string("1|x;2|y"));
  CHECK_ERROR(load(db, "t", "1\n", options), Io);
  CHECK_ERROR(load(db, "missing", "1\n", options), Catalog);
}

TEST(import_distinguishes_null_from_empty_text) {
  Database db;
  db.execute("CREATE TABLE t (a TEXT, b TEXT);");
  load(db, "t", "a,b\n,\"\"\n");
  CHECK_EQ(q(db, "SELECT a IS NULL, b IS NULL, b FROM t"), std::string("true|false|"));
}

TEST(import_creates_a_table_with_inferred_types) {
  Database db;
  CHECK_EQ(load(db, "fresh", "i,f,b,s,n,mixed\n1,1.5,true,x,,1\n2,2,FALSE,y,,z\n"), size_t{2});
  const Table* table = db.catalog().find("fresh");
  CHECK(table != nullptr);
  const auto& columns = table->columns();
  CHECK(columns[0].type == Type::Int);
  CHECK(columns[1].type == Type::Float);
  CHECK(columns[2].type == Type::Bool);
  CHECK(columns[3].type == Type::Text);
  CHECK(columns[4].type == Type::Text);
  CHECK(columns[5].type == Type::Text);
  CHECK_EQ(q(db, "SELECT i + 1, f, b, s, n, mixed FROM fresh WHERE i = 2"), std::string("3|2.0|false|y|NULL|z"));
}

TEST(import_with_header_only_creates_an_empty_table) {
  Database db;
  CHECK_EQ(load(db, "fresh", "a,b\n"), size_t{0});
  CHECK(db.catalog().find("fresh") != nullptr);
  CHECK_EQ(q(db, "SELECT COUNT(*) FROM fresh"), std::string("0"));
}

TEST(import_is_all_or_nothing) {
  Database db;
  db.execute("CREATE TABLE t (id INT PRIMARY KEY, n INT NOT NULL);"
             "INSERT INTO t VALUES (1, 1);");
  CHECK_ERROR(load(db, "t", "id,n\n2,2\nabc,3\n"), Io);
  CHECK_ERROR(load(db, "t", "id,n\n2,2\n3,\n"), Io);
  CHECK_ERROR(load(db, "t", "id,n\n2,2\n1,3\n"), Constraint);
  CHECK_ERROR(load(db, "t", "id,n\n2,2\n2,3\n"), Constraint);
  CHECK_ERROR(load(db, "t", "id\n2\n"), Io);
  CHECK_EQ(q(db, "SELECT id FROM t"), std::string("1"));
}

TEST(failed_import_leaves_no_new_table) {
  Database db;
  CHECK_ERROR(load(db, "fresh", "a,b\n1\n"), Io);
  CHECK_ERROR(load(db, "fresh", "a,b\n1,\"open\n"), Io);
  CHECK(db.catalog().find("fresh") == nullptr);
}

TEST(import_error_names_the_line) {
  Database db;
  db.execute("CREATE TABLE t (a INT);");
  try {
    load(db, "t", "a\n1\n\nbad\n");
    CHECK(false);
  } catch (const Error& e) {
    CHECK(e.kind() == ErrorKind::Io);
    CHECK_EQ(e.pos().line, 4);
  }
}

TEST(import_header_mistakes) {
  Database db;
  db.execute("CREATE TABLE t (a INT);");
  CHECK_ERROR(load(db, "t", "nope\n1\n"), Catalog);
  CHECK_ERROR(load(db, "t", "a,a\n1,2\n"), Io);
  CHECK_ERROR(load(db, "t", ""), Io);
  CHECK_ERROR(load(db, "other", "a,,c\n1,2,3\n"), Io);
}

TEST(import_custom_delimiter) {
  Database db;
  CsvOptions options;
  options.delimiter = ';';
  load(db, "t", "a;b\n1;x,y\n", options);
  CHECK_EQ(q(db, "SELECT a, b FROM t"), std::string("1|x,y"));
}

TEST(import_inside_a_transaction_rolls_back) {
  Database db;
  db.execute("CREATE TABLE t (a INT); BEGIN;");
  load(db, "t", "a\n1\n");
  load(db, "fresh", "b\n2\n");
  db.query("ROLLBACK");
  CHECK_EQ(q(db, "SELECT COUNT(*) FROM t"), std::string("0"));
  CHECK(db.catalog().find("fresh") == nullptr);
}

TEST(export_quotes_only_when_needed) {
  Database db;
  db.execute("CREATE TABLE t (a INT, b TEXT, c BOOL, d FLOAT);"
             "INSERT INTO t VALUES (1, 'x,y', TRUE, 1.5), (NULL, 'q\"r', NULL, NULL), (3, '', FALSE, 2);");
  std::ostringstream out;
  CHECK_EQ(db.exportCsv("t", out), size_t{3});
  CHECK_EQ(out.str(), std::string("a,b,c,d\n1,\"x,y\",true,1.5\n,\"q\"\"r\",,\n3,\"\",false,2.0\n"));
  CHECK_ERROR(db.exportCsv("nope", out), Catalog);
}

TEST(export_then_import_round_trip) {
  Database db;
  db.execute("CREATE TABLE t (id INT PRIMARY KEY, s TEXT, f FLOAT, b BOOL);"
             "INSERT INTO t VALUES (1, 'plain', 0.1, TRUE), (2, 'with\nnewline', 1e100, FALSE),"
             " (3, 'a,\"b\"', NULL, NULL), (4, '', -2.5, TRUE), (5, NULL, 3, FALSE);"
             "DELETE FROM t WHERE id = 1;");
  std::stringstream data;
  db.exportCsv("t", data);

  Database copy;
  copy.execute("CREATE TABLE t (id INT PRIMARY KEY, s TEXT, f FLOAT, b BOOL);");
  CHECK_EQ(copy.importCsv("t", data), size_t{4});
  std::string all = "SELECT id, s, f, b, s IS NULL FROM t ORDER BY id";
  CHECK_EQ(rowsText(copy.query(all)), rowsText(db.query(all)));
}

TEST(export_without_header_and_with_delimiter) {
  Database db;
  db.execute("CREATE TABLE t (a INT, b TEXT); INSERT INTO t VALUES (1, 'x;y');");
  std::ostringstream out;
  CsvOptions options;
  options.header = false;
  options.delimiter = ';';
  db.exportCsv("t", out, options);
  CHECK_EQ(out.str(), std::string("1;\"x;y\"\n"));
}

TEST_MAIN
