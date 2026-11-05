#pragma once

#include <cstddef>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "minisql/ast.h"
#include "minisql/catalog.h"
#include "minisql/csv.h"

namespace minisql {

// What one statement produced. A query has column names and rows; anything else
// has only a status line such as "INSERT 3".
struct Result {
  std::vector<std::string> columns;
  std::vector<Row> rows;
  size_t affected = 0;
  std::string status;

  bool isQuery() const { return !columns.empty(); }
};

class Database {
 public:
  // Runs every statement in the script and returns their results in order. The
  // whole script is parsed first, so a syntax error runs nothing. A later
  // failure does not undo statements that already finished.
  std::vector<Result> execute(std::string_view sql);
  // Convenience for one statement: returns the last statement's result.
  Result query(std::string_view sql);
  // Runs one parsed statement. Each statement is atomic: if it throws, the
  // database is as it was before (an open transaction stays open).
  Result run(const ast::Statement& statement);

  bool inTransaction() const { return snapshot_.has_value(); }
  const Catalog& catalog() const { return catalog_; }

  // Writes a SQL script that recreates the whole database.
  void dump(std::ostream& out) const;

  // Loads CSV into a table, all rows or none. When the table does not exist and
  // the input has a header, the table is created with column types inferred
  // from the data. Returns the number of rows imported.
  size_t importCsv(const std::string& table, std::istream& in, const CsvOptions& options = {});
  size_t exportCsv(const std::string& table, std::ostream& out, const CsvOptions& options = {}) const;

 private:
  Catalog catalog_;
  std::optional<Catalog::Snapshot> snapshot_;  // set while a transaction is open
};

}  // namespace minisql
