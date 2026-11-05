#pragma once

#include <memory>
#include <string>
#include <vector>

#include "minisql/ast.h"
#include "minisql/catalog.h"
#include "operators.h"

namespace minisql {

struct SelectPlan {
  OperatorPtr root;
  std::vector<std::string> columns;
};

// Resolves names and picks access paths. Everything that can be wrong with the
// query's names and shape is reported here, before any row is read. The plan
// keeps pointers into the catalog's tables and must not outlive a change to them.
SelectPlan planSelect(const ast::Select& select, const Catalog& catalog);

// The scan used by UPDATE and DELETE: one table, the WHERE clause (may be null)
// pushed into the scan, using an index when the clause allows it.
std::unique_ptr<TableAccess> planTableAccess(const Table& table, const ast::Expr* where);

}  // namespace minisql
