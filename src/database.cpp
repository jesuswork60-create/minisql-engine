#include "minisql/database.h"

#include <ostream>
#include <utility>

#include "executor.h"
#include "minisql/parser.h"

namespace minisql {

std::vector<Result> Database::execute(std::string_view sql) {
  std::vector<ast::Statement> statements = parseScript(sql);
  std::vector<Result> results;
  results.reserve(statements.size());
  for (const ast::Statement& statement : statements) results.push_back(run(statement));
  return results;
}

Result Database::query(std::string_view sql) {
  std::vector<Result> results = execute(sql);
  return results.empty() ? Result{} : std::move(results.back());
}

Result Database::run(const ast::Statement& statement) {
  return locating(statement.pos, [&]() -> Result {
    Result result;
    if (std::holds_alternative<ast::Begin>(statement.node)) {
      if (snapshot_) throw Error(ErrorKind::Transaction, "a transaction is already open");
      snapshot_ = catalog_.snapshot();
      result.status = "BEGIN";
    } else if (std::holds_alternative<ast::Commit>(statement.node)) {
      if (!snapshot_) throw Error(ErrorKind::Transaction, "no transaction is open");
      snapshot_.reset();
      result.status = "COMMIT";
    } else if (std::holds_alternative<ast::Rollback>(statement.node)) {
      if (!snapshot_) throw Error(ErrorKind::Transaction, "no transaction is open");
      catalog_.restore(std::move(*snapshot_));
      snapshot_.reset();
      result.status = "ROLLBACK";
    } else {
      result = executeStatement(statement, catalog_);
    }
    return result;
  });
}

void Database::dump(std::ostream& out) const {
  out << "BEGIN;\n";
  for (const Table* table : catalog_.tables()) {
    out << table->createSql() << '\n';
    for (RowId id = 0; id < table->slotCount(); ++id) {
      if (!table->isLive(id)) continue;
      out << "INSERT INTO " << ast::quoteIdentifier(table->name()) << " VALUES (";
      const Row& row = table->row(id);
      for (size_t i = 0; i < row.size(); ++i) {
        if (i) out << ", ";
        out << row[i].toSql();
      }
      out << ");\n";
    }
    for (const std::string& statement : table->indexSql()) out << statement << '\n';
  }
  out << "COMMIT;\n";
}

}  // namespace minisql
