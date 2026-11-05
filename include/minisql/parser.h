#pragma once

#include <string_view>
#include <vector>

#include "minisql/ast.h"

namespace minisql {

// Parses a script of zero or more statements separated by semicolons. The
// semicolon after the last statement is optional. Throws a syntax Error with
// the position of the offending token.
std::vector<ast::Statement> parseScript(std::string_view sql);

// Parses one expression and nothing else.
ast::ExprPtr parseExpression(std::string_view sql);

}  // namespace minisql
