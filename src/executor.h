#pragma once

#include "minisql/ast.h"
#include "minisql/catalog.h"
#include "minisql/database.h"

namespace minisql {

// Runs every statement kind except the transaction ones, which belong to Database.
Result executeStatement(const ast::Statement& statement, Catalog& catalog);

}  // namespace minisql
