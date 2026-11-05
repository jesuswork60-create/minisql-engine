#pragma once

#include <memory>
#include <string>
#include <vector>

#include "minisql/ast.h"
#include "minisql/catalog.h"

namespace minisql {

// An expression with its column references resolved to row slots, ready to run.
class BoundExpr {
 public:
  virtual ~BoundExpr() = default;
  virtual Value eval(const Row& row) const = 0;
};
using BoundPtr = std::unique_ptr<BoundExpr>;

BoundPtr makeSlotExpr(size_t slot);

struct ScopeColumn {
  std::string qualifier;  // the table's alias, or its name when it has no alias
  std::string name;
  int table;  // position of the table in the FROM clause
};

// The columns visible to an expression, laid out as they appear in a row.
class Scope {
 public:
  void addTable(const Table& table, const std::string& qualifier, int tablePosition);

  struct Resolved {
    size_t slot;
    int table;
  };
  // Throws a catalog error for unknown or ambiguous columns.
  Resolved resolve(const ast::ColumnRef& ref, SourcePos pos) const;

  const std::vector<ScopeColumn>& columns() const { return columns_; }
  bool hasQualifier(const std::string& qualifier) const;

 private:
  std::vector<ScopeColumn> columns_;
};

// Output layout of an aggregate operator: group keys, then aggregate results.
// Above the aggregate, an expression can only use these, matched by meaning.
struct GroupedOutput {
  const Scope* source = nullptr;
  std::vector<std::string> keys;  // canonicalExpr of each output slot
};

struct BindContext {
  const Scope* scope = nullptr;           // null: column references are an error
  const char* clause = "this context";    // named in error messages
  const GroupedOutput* grouped = nullptr;  // set when binding above an aggregate
};

BoundPtr bindExpr(const ast::Expr& expr, const BindContext& context);

// Text that is equal for expressions that mean the same thing in the scope,
// whatever their spelling: t.a and a print alike when they are the same column.
std::string canonicalExpr(const ast::Expr& expr, const Scope& scope);

bool isAggregateCall(const ast::Expr& expr);
bool containsAggregate(const ast::Expr& expr);
// Adds each outermost aggregate call found in expr.
void collectAggregates(const ast::Expr& expr, std::vector<const ast::Expr*>& out);

// Runs a column-free expression once; for DEFAULT, LIMIT and friends.
Value evalConstant(const ast::Expr& expr, const char* clause);

// A conjunction of bound conditions. A row passes only if every term is TRUE;
// FALSE and NULL both reject it.
class Predicate {
 public:
  void add(BoundPtr term, std::string text, SourcePos pos);
  bool empty() const { return terms_.empty(); }
  bool test(const Row& row) const;
  const std::string& text() const { return text_; }

 private:
  struct Term {
    BoundPtr expr;
    SourcePos pos;
  };
  std::vector<Term> terms_;
  std::string text_;
};

}  // namespace minisql
