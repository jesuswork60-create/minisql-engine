#include "minisql/ast.h"

#include "minisql/lexer.h"
#include "overloaded.h"

namespace minisql::ast {

namespace {

// Binding strength, tightest last; mirrors the parser's levels.
enum Precedence { kOr = 1, kAnd, kNot, kPredicate, kConcat, kAdditive, kMultiplicative, kUnary, kPrimary };

Precedence binaryPrecedence(BinaryOp op) {
  switch (op) {
    case BinaryOp::Or: return kOr;
    case BinaryOp::And: return kAnd;
    case BinaryOp::Concat: return kConcat;
    case BinaryOp::Add:
    case BinaryOp::Sub: return kAdditive;
    case BinaryOp::Mul:
    case BinaryOp::Div:
    case BinaryOp::Mod: return kMultiplicative;
    default: return kPredicate;
  }
}

const char* binarySymbol(BinaryOp op) {
  switch (op) {
    case BinaryOp::Add: return "+";
    case BinaryOp::Sub: return "-";
    case BinaryOp::Mul: return "*";
    case BinaryOp::Div: return "/";
    case BinaryOp::Mod: return "%";
    case BinaryOp::Concat: return "||";
    case BinaryOp::Eq: return "=";
    case BinaryOp::Ne: return "<>";
    case BinaryOp::Lt: return "<";
    case BinaryOp::Le: return "<=";
    case BinaryOp::Gt: return ">";
    case BinaryOp::Ge: return ">=";
    case BinaryOp::And: return "AND";
    case BinaryOp::Or: return "OR";
  }
  return "?";
}

Precedence precedenceOf(const Expr& expr) {
  return std::visit(overloaded{
                        [](const Unary& u) { return u.op == UnaryOp::Not ? kNot : kUnary; },
                        [](const Binary& b) { return binaryPrecedence(b.op); },
                        [](const IsNull&) { return kPredicate; },
                        [](const InList&) { return kPredicate; },
                        [](const Between&) { return kPredicate; },
                        [](const Like&) { return kPredicate; },
                        [](const auto&) { return kPrimary; },
                    },
                    expr.node);
}

class Printer {
 public:
  explicit Printer(const ColumnPrinter* columns) : columns_(columns) {}

  std::string print(const Expr& expr, int minPrecedence = 0) const {
    std::string text = printNode(expr);
    return precedenceOf(expr) < minPrecedence ? "(" + text + ")" : text;
  }

 private:
  std::string printNode(const Expr& expr) const {
    return std::visit(
        overloaded{
            [&](const Literal& n) { return n.value.toSql(); },
            [&](const ColumnRef& n) {
              if (columns_) return (*columns_)(n, expr.pos);
              std::string name = quoteIdentifier(n.name);
              return n.qualifier.empty() ? name : quoteIdentifier(n.qualifier) + "." + name;
            },
            [&](const Unary& n) {
              if (n.op == UnaryOp::Not) return "NOT " + print(*n.operand, kNot);
              std::string operand = print(*n.operand, kUnary);
              // Never glue two minus signs together: -- would start a comment.
              if (operand[0] == '-' || operand[0] == '+') operand.insert(0, " ");
              return (n.op == UnaryOp::Negate ? "-" : "+") + operand;
            },
            [&](const Binary& n) {
              int own = binaryPrecedence(n.op);
              return print(*n.lhs, own) + " " + binarySymbol(n.op) + " " + print(*n.rhs, own + 1);
            },
            [&](const IsNull& n) {
              return print(*n.operand, kConcat) + (n.negated ? " IS NOT NULL" : " IS NULL");
            },
            [&](const InList& n) {
              std::string text = print(*n.operand, kConcat) + (n.negated ? " NOT IN (" : " IN (");
              for (size_t i = 0; i < n.items.size(); ++i) {
                if (i) text += ", ";
                text += print(*n.items[i]);
              }
              return text + ")";
            },
            [&](const Between& n) {
              return print(*n.operand, kConcat) + (n.negated ? " NOT BETWEEN " : " BETWEEN ") +
                     print(*n.low, kConcat) + " AND " + print(*n.high, kConcat);
            },
            [&](const Like& n) {
              return print(*n.operand, kConcat) + (n.negated ? " NOT LIKE " : " LIKE ") +
                     print(*n.pattern, kConcat);
            },
            [&](const Cast& n) {
              return "CAST(" + print(*n.operand) + " AS " + std::string(typeName(n.target)) + ")";
            },
            [&](const Call& n) {
              if (n.star) return n.name + "(*)";
              std::string text = n.name + "(";
              for (size_t i = 0; i < n.args.size(); ++i) {
                if (i) text += ", ";
                text += print(*n.args[i]);
              }
              return text + ")";
            },
        },
        expr.node);
  }

  const ColumnPrinter* columns_;
};

}  // namespace

std::string quoteIdentifier(const std::string& name) {
  bool plain = !name.empty() && !isReservedWord(name) && !(name[0] >= '0' && name[0] <= '9');
  for (char c : name) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    plain = plain && ok;
  }
  if (plain) return name;
  std::string out = "\"";
  for (char c : name) {
    if (c == '"') out += '"';
    out += c;
  }
  return out + "\"";
}

std::string printExpr(const Expr& expr, const ColumnPrinter* columns) { return Printer(columns).print(expr); }

void forEachChild(const Expr& expr, const std::function<void(const Expr&)>& fn) {
  std::visit(overloaded{
                 [](const Literal&) {},
                 [](const ColumnRef&) {},
                 [&](const Unary& n) { fn(*n.operand); },
                 [&](const Binary& n) {
                   fn(*n.lhs);
                   fn(*n.rhs);
                 },
                 [&](const IsNull& n) { fn(*n.operand); },
                 [&](const InList& n) {
                   fn(*n.operand);
                   for (const auto& item : n.items) fn(*item);
                 },
                 [&](const Between& n) {
                   fn(*n.operand);
                   fn(*n.low);
                   fn(*n.high);
                 },
                 [&](const Like& n) {
                   fn(*n.operand);
                   fn(*n.pattern);
                 },
                 [&](const Cast& n) { fn(*n.operand); },
                 [&](const Call& n) {
                   for (const auto& arg : n.args) fn(*arg);
                 },
             },
             expr.node);
}

}  // namespace minisql::ast
