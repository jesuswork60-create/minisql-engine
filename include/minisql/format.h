#pragma once

#include <iosfwd>

#include "minisql/database.h"

namespace minisql {

// Writes a query as an aligned ASCII table with a row count, or a non-query as
// its status line. Numbers align right, text left, and NULL shows as NULL.
void printResult(const Result& result, std::ostream& out);

}  // namespace minisql
