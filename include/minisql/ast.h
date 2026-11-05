#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "minisql/error.h"
#include "minisql/value.h"

namespace minisql::ast {

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Literal {
  Value value;
};
struct ColumnRef {
  std::string qualifier;  // empty when unqualified
  std::string name;
};
enum class UnaryOp { Negate, Plus, Not };
struct Unary {
  UnaryOp op;
  ExprPtr operand;
};
enum class BinaryOp { Add, Sub, Mul, Div, Mod, Concat, Eq, Ne, Lt, Le, Gt, Ge, And, Or };
struct Binary {
  BinaryOp op;
  ExprPtr lhs;
  ExprPtr rhs;
};
struct IsNull {
  ExprPtr operand;
  bool negated;
};
struct InList {
  ExprPtr operand;
  std::vector<ExprPtr> items;
  bool negated;
};
struct Between {
  ExprPtr operand;
  ExprPtr low;
  ExprPtr high;
  bool negated;
};
struct Like {
  ExprPtr operand;
  ExprPtr pattern;
  bool negated;
};
struct Cast {
  ExprPtr operand;
  Type target;
};
struct Call {
  std::string name;  // upper-cased
  std::vector<ExprPtr> args;
  bool star;  // COUNT(*)
};

struct Expr {
  using Node = std::variant<Literal, ColumnRef, Unary, Binary, IsNull, InList, Between, Like, Cast, Call>;
  Node node;
  SourcePos pos;
  int height;  // of this subtree; the parser caps it so recursive walks stay shallow
};

// How a column is written by printExpr. Used to compare expressions by meaning:
// printing t.a and a as the same resolved slot makes them textually equal.
using ColumnPrinter = std::function<std::string(const ColumnRef&, SourcePos)>;

// Prints valid SQL with only the parentheses the grammar needs.
std::string printExpr(const Expr& expr, const ColumnPrinter* columns = nullptr);
// The name itself when it is a plain identifier, otherwise "quoted".
std::string quoteIdentifier(const std::string& name);
// Calls fn on each direct child expression.
void forEachChild(const Expr& expr, const std::function<void(const Expr&)>& fn);

struct ColumnDef {
  std::string name;
  Type type;
  bool notNull;
  bool primaryKey;
  ExprPtr defaultExpr;  // may be null
  SourcePos pos;
};
struct CreateTable {
  std::string name;
  bool ifNotExists;
  std::vector<ColumnDef> columns;
};
struct DropTable {
  std::string name;
  bool ifExists;
};
struct CreateIndex {
  std::string name;
  std::string table;
  std::string column;
  bool unique;
  bool ifNotExists;
  SourcePos columnPos;
};
struct DropIndex {
  std::string name;
  bool ifExists;
};
struct Insert {
  std::string table;
  std::vector<std::string> columns;  // empty means all, in declared order
  std::vector<std::vector<ExprPtr>> rows;
  std::vector<SourcePos> rowPositions;
};
struct Assignment {
  std::string column;
  ExprPtr value;
  SourcePos pos;
};
struct Update {
  std::string table;
  std::vector<Assignment> assignments;
  ExprPtr where;  // may be null
};
struct Delete {
  std::string table;
  ExprPtr where;  // may be null
};

struct SelectItem {
  ExprPtr expr;          // null for * and t.*
  std::string alias;     // may be empty
  std::string starTable; // the t of t.*; empty for a bare *
  SourcePos pos;
};
struct TableRef {
  std::string name;
  std::string alias;  // may be empty
  SourcePos pos;
};
struct Join {
  TableRef table;
  ExprPtr on;  // null for CROSS JOIN and comma joins
};
struct OrderTerm {
  ExprPtr expr;
  bool descending;
};
struct Select {
  std::vector<SelectItem> items;
  std::optional<TableRef> from;
  std::vector<Join> joins;
  ExprPtr where;
  std::vector<ExprPtr> groupBy;
  ExprPtr having;
  std::vector<OrderTerm> orderBy;
  ExprPtr limit;
  ExprPtr offset;
};

struct Begin {};
struct Commit {};
struct Rollback {};

struct Statement;
struct Explain {
  std::unique_ptr<Statement> inner;
};

struct Statement {
  using Node = std::variant<CreateTable, DropTable, CreateIndex, DropIndex, Insert, Update, Delete, Select,
                            Begin, Commit, Rollback, Explain>;
  Node node;
  SourcePos pos;
};

}  // namespace minisql::ast
