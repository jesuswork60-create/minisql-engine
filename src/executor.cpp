#include "executor.h"

#include <algorithm>
#include <set>
#include <utility>

#include "binder.h"
#include "overloaded.h"
#include "planner.h"

namespace minisql {

namespace {

Result statusResult(std::string status, size_t affected = 0) {
  Result result;
  result.status = std::move(status);
  result.affected = affected;
  return result;
}

Column buildColumn(const ast::ColumnDef& def, const std::string& tableName) {
  Column column;
  column.name = def.name;
  column.type = def.type;
  column.primaryKey = def.primaryKey;
  column.notNull = def.notNull || def.primaryKey;
  if (def.defaultExpr) {
    Value value = evalConstant(*def.defaultExpr, "DEFAULT");
    locating(def.defaultExpr->pos, [&] {
      auto converted = assignValue(value, def.type);
      if (!converted) {
        throw Error(ErrorKind::Type, "default for " + tableName + "." + def.name + " must be " +
                                         std::string(typeName(def.type)) + ", got " + value.toSql());
      }
      if (converted->isNull() && column.notNull) {
        throw Error(ErrorKind::Constraint, "default for NOT NULL column " + tableName + "." + def.name + " is NULL");
      }
      column.hasDefault = true;
      column.defaultValue = *converted;
      return 0;
    });
  }
  return column;
}

Result createTable(const ast::CreateTable& create, Catalog& catalog) {
  if (catalog.find(create.name)) {
    if (create.ifNotExists) return statusResult("CREATE TABLE skipped");
    throw Error(ErrorKind::Catalog, "table already exists: " + create.name);
  }
  std::vector<Column> columns;
  std::set<std::string> seen;
  bool hasPrimaryKey = false;
  for (const ast::ColumnDef& def : create.columns) {
    if (!seen.insert(toLowerAscii(def.name)).second) {
      throw Error(ErrorKind::Catalog, "duplicate column name: " + def.name, def.pos);
    }
    if (def.primaryKey && hasPrimaryKey) {
      throw Error(ErrorKind::Catalog, "table " + create.name + " has more than one primary key", def.pos);
    }
    hasPrimaryKey = hasPrimaryKey || def.primaryKey;
    columns.push_back(buildColumn(def, create.name));
  }
  Table table(create.name, std::move(columns));
  if (hasPrimaryKey && catalog.tableWithIndex(table.indexes().front().name())) {
    throw Error(ErrorKind::Catalog, "index already exists: " + table.indexes().front().name());
  }
  catalog.addTable(std::move(table));
  return statusResult("CREATE TABLE");
}

Result dropTable(const ast::DropTable& drop, Catalog& catalog) {
  if (!catalog.find(drop.name)) {
    if (drop.ifExists) return statusResult("DROP TABLE skipped");
    throw Error(ErrorKind::Catalog, "no such table: " + drop.name);
  }
  catalog.dropTable(drop.name);
  return statusResult("DROP TABLE");
}

Result createIndex(const ast::CreateIndex& create, Catalog& catalog) {
  if (catalog.tableWithIndex(create.name)) {
    if (create.ifNotExists) return statusResult("CREATE INDEX skipped");
    throw Error(ErrorKind::Catalog, "index already exists: " + create.name);
  }
  const Table& table = catalog.require(create.table);
  auto column = table.columnIndex(create.column);
  if (!column) {
    throw Error(ErrorKind::Catalog, "no such column: " + create.table + "." + create.column, create.columnPos);
  }
  // A duplicate in existing data fails inside addIndex, before the index is attached.
  catalog.mutableTable(create.table).addIndex(create.name, *column, create.unique);
  return statusResult("CREATE INDEX");
}

Result dropIndex(const ast::DropIndex& drop, Catalog& catalog) {
  const Table* owner = catalog.tableWithIndex(drop.name);
  if (!owner) {
    if (drop.ifExists) return statusResult("DROP INDEX skipped");
    throw Error(ErrorKind::Catalog, "no such index: " + drop.name);
  }
  if (owner->findIndex(drop.name)->primary()) {
    throw Error(ErrorKind::Catalog, "index " + drop.name + " enforces a primary key and cannot be dropped");
  }
  catalog.mutableTable(owner->name()).dropIndex(drop.name);
  return statusResult("DROP INDEX");
}

Result insertInto(const ast::Insert& insert, Catalog& catalog) {
  const Table& table = catalog.require(insert.table);
  const auto& columns = table.columns();

  // targets[i] is the table column receiving the i-th value of each VALUES row.
  std::vector<size_t> targets;
  if (insert.columns.empty()) {
    for (size_t i = 0; i < columns.size(); ++i) targets.push_back(i);
  } else {
    std::set<size_t> seen;
    for (const std::string& name : insert.columns) {
      auto column = table.columnIndex(name);
      if (!column) throw Error(ErrorKind::Catalog, "no such column: " + insert.table + "." + name);
      if (!seen.insert(*column).second) throw Error(ErrorKind::Catalog, "column named twice: " + name);
      targets.push_back(*column);
    }
  }

  std::vector<Row> rows;
  rows.reserve(insert.rows.size());
  for (size_t r = 0; r < insert.rows.size(); ++r) {
    const auto& exprs = insert.rows[r];
    SourcePos rowPos = insert.rowPositions[r];
    if (exprs.size() != targets.size()) {
      throw Error(ErrorKind::Syntax,
                  "row has " + std::to_string(exprs.size()) + " values but " + std::to_string(targets.size()) +
                      " were expected",
                  rowPos);
    }
    Row row(columns.size());
    std::vector<bool> given(columns.size(), false);
    for (size_t i = 0; i < exprs.size(); ++i) {
      Value value = evalConstant(*exprs[i], "VALUES");
      row[targets[i]] = locating(exprs[i]->pos, [&] { return table.normalize(targets[i], value); });
      given[targets[i]] = true;
    }
    for (size_t c = 0; c < columns.size(); ++c) {
      if (given[c]) continue;
      Value fallback = columns[c].hasDefault ? columns[c].defaultValue : Value::Null();
      row[c] = locating(rowPos, [&] { return table.normalize(c, fallback); });
    }
    rows.push_back(std::move(row));
  }

  size_t count = rows.size();
  catalog.mutableTable(insert.table).insertRows(std::move(rows));
  return statusResult("INSERT " + std::to_string(count), count);
}

struct BoundAssignment {
  size_t column;
  BoundPtr value;
  SourcePos pos;
};

std::vector<BoundAssignment> bindAssignments(const ast::Update& update, const Table& table, const Scope& scope) {
  BindContext context;
  context.scope = &scope;
  context.clause = "UPDATE";
  std::vector<BoundAssignment> assignments;
  std::set<size_t> seen;
  for (const ast::Assignment& assignment : update.assignments) {
    auto column = table.columnIndex(assignment.column);
    if (!column) {
      throw Error(ErrorKind::Catalog, "no such column: " + update.table + "." + assignment.column, assignment.pos);
    }
    if (!seen.insert(*column).second) {
      throw Error(ErrorKind::Catalog, "column assigned twice: " + assignment.column, assignment.pos);
    }
    assignments.push_back(BoundAssignment{*column, bindExpr(*assignment.value, context), assignment.value->pos});
  }
  return assignments;
}

Result updateTable(const ast::Update& update, Catalog& catalog) {
  const Table& table = catalog.require(update.table);
  Scope scope;
  scope.addTable(table, table.name(), 0);
  auto assignments = bindAssignments(update, table, scope);
  auto access = planTableAccess(table, update.where.get());

  // New rows are computed from the old ones up front, so SET a = b, b = a swaps
  // and any failure happens while the table is still untouched.
  std::vector<RowUpdate> updates;
  access->open(Row{});
  Row row;
  while (access->next(row)) {
    Row changed = row;
    for (const BoundAssignment& assignment : assignments) {
      Value value = assignment.value->eval(row);
      changed[assignment.column] = locating(assignment.pos, [&] { return table.normalize(assignment.column, value); });
    }
    updates.push_back(RowUpdate{access->rowId(), std::move(changed)});
  }

  size_t count = updates.size();
  access.reset();  // holds a reference to the table that mutableTable may replace
  catalog.mutableTable(update.table).applyUpdates(std::move(updates));
  return statusResult("UPDATE " + std::to_string(count), count);
}

Result deleteFrom(const ast::Delete& del, Catalog& catalog) {
  const Table& table = catalog.require(del.table);
  auto access = planTableAccess(table, del.where.get());

  std::vector<RowId> ids;
  access->open(Row{});
  Row row;
  while (access->next(row)) ids.push_back(access->rowId());

  access.reset();
  catalog.mutableTable(del.table).deleteRows(ids);
  return statusResult("DELETE " + std::to_string(ids.size()), ids.size());
}

Result runSelect(const ast::Select& select, const Catalog& catalog) {
  SelectPlan plan = planSelect(select, catalog);
  plan.root->open(Row{});
  Result result;
  result.columns = std::move(plan.columns);
  result.rows = drain(*plan.root);
  result.status = "SELECT " + std::to_string(result.rows.size());
  return result;
}

Result planLines(std::vector<std::string> lines) {
  Result result;
  result.columns = {"plan"};
  for (std::string& line : lines) result.rows.push_back(Row{Value::Text(std::move(line))});
  result.status = "EXPLAIN";
  return result;
}

Result explainStatement(const ast::Statement& statement, const Catalog& catalog) {
  auto withHeader = [](std::string header, const Operator& access) {
    std::vector<std::string> lines{std::move(header)};
    auto nested = renderPlan(access, 1);
    lines.insert(lines.end(), nested.begin(), nested.end());
    return lines;
  };
  return std::visit(
      overloaded{
          [&](const ast::Select& select) { return planLines(renderPlan(*planSelect(select, catalog).root)); },
          [&](const ast::Update& update) {
            const Table& table = catalog.require(update.table);
            Scope scope;
            scope.addTable(table, table.name(), 0);
            bindAssignments(update, table, scope);
            return planLines(withHeader("Update " + table.name(), *planTableAccess(table, update.where.get())));
          },
          [&](const ast::Delete& del) {
            const Table& table = catalog.require(del.table);
            return planLines(withHeader("Delete " + table.name(), *planTableAccess(table, del.where.get())));
          },
          [&](const ast::Insert& insert) {
            const Table& table = catalog.require(insert.table);
            return planLines({"Insert " + table.name() + " rows=" + std::to_string(insert.rows.size())});
          },
          [](const auto&) -> Result { throw Error(ErrorKind::Syntax, "this statement cannot be explained"); },
      },
      statement.node);
}

}  // namespace

Result executeStatement(const ast::Statement& statement, Catalog& catalog) {
  return std::visit(
      overloaded{
          [&](const ast::CreateTable& n) { return createTable(n, catalog); },
          [&](const ast::DropTable& n) { return dropTable(n, catalog); },
          [&](const ast::CreateIndex& n) { return createIndex(n, catalog); },
          [&](const ast::DropIndex& n) { return dropIndex(n, catalog); },
          [&](const ast::Insert& n) { return insertInto(n, catalog); },
          [&](const ast::Update& n) { return updateTable(n, catalog); },
          [&](const ast::Delete& n) { return deleteFrom(n, catalog); },
          [&](const ast::Select& n) { return runSelect(n, catalog); },
          [&](const ast::Explain& n) { return explainStatement(*n.inner, catalog); },
          [](const auto&) -> Result { throw Error(ErrorKind::Transaction, "unexpected transaction statement"); },
      },
      statement.node);
}

}  // namespace minisql
