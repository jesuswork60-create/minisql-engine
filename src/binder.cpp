#include "binder.h"

#include <algorithm>
#include <utility>

#include "functions.h"
#include "overloaded.h"

namespace minisql {

namespace {

// ---- bound expression nodes -------------------------------------------------

class LiteralExpr : public BoundExpr {
 public:
  explicit LiteralExpr(Value value) : value_(std::move(value)) {}
  Value eval(const Row&) const override { return value_; }

 private:
  Value value_;
};

class SlotExpr : public BoundExpr {
 public:
  explicit SlotExpr(size_t slot) : slot_(slot) {}
  Value eval(const Row& row) const override { return row[slot_]; }

 private:
  size_t slot_;
};

class UnaryExpr : public BoundExpr {
 public:
  using Fn = Value (*)(const Value&);
  UnaryExpr(Fn fn, BoundPtr operand, SourcePos pos) : fn_(fn), operand_(std::move(operand)), pos_(pos) {}
  Value eval(const Row& row) const override {
    Value operand = operand_->eval(row);
    return locating(pos_, [&] { return fn_(operand); });
  }

 private:
  Fn fn_;
  BoundPtr operand_;
  SourcePos pos_;
};

class ArithmeticExpr : public BoundExpr {
 public:
  using Fn = Value (*)(const Value&, const Value&);
  ArithmeticExpr(Fn fn, BoundPtr lhs, BoundPtr rhs, SourcePos pos)
      : fn_(fn), lhs_(std::move(lhs)), rhs_(std::move(rhs)), pos_(pos) {}
  Value eval(const Row& row) const override {
    Value a = lhs_->eval(row);
    Value b = rhs_->eval(row);
    return locating(pos_, [&] { return fn_(a, b); });
  }

 private:
  Fn fn_;
  BoundPtr lhs_, rhs_;
  SourcePos pos_;
};

class CompareExpr : public BoundExpr {
 public:
  CompareExpr(CompareOp op, BoundPtr lhs, BoundPtr rhs, SourcePos pos)
      : op_(op), lhs_(std::move(lhs)), rhs_(std::move(rhs)), pos_(pos) {}
  Value eval(const Row& row) const override {
    Value a = lhs_->eval(row);
    Value b = rhs_->eval(row);
    return locating(pos_, [&] { return compareSql(op_, a, b); });
  }

 private:
  CompareOp op_;
  BoundPtr lhs_, rhs_;
  SourcePos pos_;
};

// AND and OR short-circuit only on their deciding value: FALSE for AND, TRUE
// for OR. A NULL left side still needs the right side to settle the answer.
class LogicExpr : public BoundExpr {
 public:
  LogicExpr(bool isAnd, BoundPtr lhs, BoundPtr rhs, SourcePos pos)
      : isAnd_(isAnd), lhs_(std::move(lhs)), rhs_(std::move(rhs)), pos_(pos) {}
  Value eval(const Row& row) const override {
    return locating(pos_, [&] {
      std::optional<bool> left = truthOf(lhs_->eval(row));
      if (left == !isAnd_) return Value::Bool(!isAnd_);
      std::optional<bool> right = truthOf(rhs_->eval(row));
      if (right == !isAnd_) return Value::Bool(!isAnd_);
      if (!left || !right) return Value::Null();
      return Value::Bool(isAnd_);
    });
  }

 private:
  bool isAnd_;
  BoundPtr lhs_, rhs_;
  SourcePos pos_;
};

class IsNullExpr : public BoundExpr {
 public:
  IsNullExpr(BoundPtr operand, bool negated) : operand_(std::move(operand)), negated_(negated) {}
  Value eval(const Row& row) const override { return Value::Bool(operand_->eval(row).isNull() != negated_); }

 private:
  BoundPtr operand_;
  bool negated_;
};

class InExpr : public BoundExpr {
 public:
  InExpr(BoundPtr operand, std::vector<BoundPtr> items, bool negated, SourcePos pos)
      : operand_(std::move(operand)), items_(std::move(items)), negated_(negated), pos_(pos) {}
  Value eval(const Row& row) const override {
    Value operand = operand_->eval(row);
    if (operand.isNull()) return Value::Null();
    bool sawNull = false;
    for (const BoundPtr& item : items_) {
      Value candidate = item->eval(row);
      if (candidate.isNull()) {
        sawNull = true;
      } else if (locating(pos_, [&] { return compareValues(operand, candidate); }) == 0) {
        return Value::Bool(!negated_);
      }
    }
    return sawNull ? Value::Null() : Value::Bool(negated_);
  }

 private:
  BoundPtr operand_;
  std::vector<BoundPtr> items_;
  bool negated_;
  SourcePos pos_;
};

class BetweenExpr : public BoundExpr {
 public:
  BetweenExpr(BoundPtr operand, BoundPtr low, BoundPtr high, bool negated, SourcePos pos)
      : operand_(std::move(operand)), low_(std::move(low)), high_(std::move(high)), negated_(negated), pos_(pos) {}
  Value eval(const Row& row) const override {
    Value operand = operand_->eval(row);
    Value low = low_->eval(row);
    Value high = high_->eval(row);
    return locating(pos_, [&] {
      // Exactly x >= low AND x <= high under three-valued logic, so a NULL
      // bound can still give FALSE when the other bound already rules x out.
      std::optional<bool> aboveLow = truthOf(compareSql(CompareOp::Ge, operand, low));
      std::optional<bool> belowHigh = truthOf(compareSql(CompareOp::Le, operand, high));
      std::optional<bool> inside;
      if (aboveLow == false || belowHigh == false) {
        inside = false;
      } else if (aboveLow && belowHigh) {
        inside = true;
      }
      if (!inside) return Value::Null();
      return Value::Bool(*inside != negated_);
    });
  }

 private:
  BoundPtr operand_, low_, high_;
  bool negated_;
  SourcePos pos_;
};

class LikeExpr : public BoundExpr {
 public:
  LikeExpr(BoundPtr operand, BoundPtr pattern, bool negated, SourcePos pos)
      : operand_(std::move(operand)), pattern_(std::move(pattern)), negated_(negated), pos_(pos) {}
  Value eval(const Row& row) const override {
    Value operand = operand_->eval(row);
    Value pattern = pattern_->eval(row);
    if (operand.isNull() || pattern.isNull()) return Value::Null();
    if (!operand.isText() || !pattern.isText()) {
      throw Error(ErrorKind::Type, "LIKE expects TEXT, got " + operand.typeLabel() + " and " + pattern.typeLabel(),
                  pos_);
    }
    return Value::Bool(likeMatch(operand.asText(), pattern.asText()) != negated_);
  }

 private:
  BoundPtr operand_, pattern_;
  bool negated_;
  SourcePos pos_;
};

class CastExpr : public BoundExpr {
 public:
  CastExpr(BoundPtr operand, Type target, SourcePos pos) : operand_(std::move(operand)), target_(target), pos_(pos) {}
  Value eval(const Row& row) const override {
    Value operand = operand_->eval(row);
    return locating(pos_, [&] { return castValue(operand, target_); });
  }

 private:
  BoundPtr operand_;
  Type target_;
  SourcePos pos_;
};

class CallExpr : public BoundExpr {
 public:
  CallExpr(const ScalarFunction* function, std::vector<BoundPtr> args, SourcePos pos)
      : function_(function), args_(std::move(args)), pos_(pos) {}
  Value eval(const Row& row) const override {
    std::vector<Value> values;
    values.reserve(args_.size());
    for (const BoundPtr& arg : args_) values.push_back(arg->eval(row));
    return locating(pos_, [&] { return function_->call(values); });
  }

 private:
  const ScalarFunction* function_;
  std::vector<BoundPtr> args_;
  SourcePos pos_;
};

class CoalesceExpr : public BoundExpr {
 public:
  explicit CoalesceExpr(std::vector<BoundPtr> args) : args_(std::move(args)) {}
  Value eval(const Row& row) const override {
    for (const BoundPtr& arg : args_) {
      Value value = arg->eval(row);
      if (!value.isNull()) return value;
    }
    return Value::Null();
  }

 private:
  std::vector<BoundPtr> args_;
};

// ---- binding ----------------------------------------------------------------

std::vector<BoundPtr> bindAll(const std::vector<ast::ExprPtr>& exprs, const BindContext& context) {
  std::vector<BoundPtr> bound;
  bound.reserve(exprs.size());
  for (const auto& expr : exprs) bound.push_back(bindExpr(*expr, context));
  return bound;
}

BoundPtr bindUnary(const ast::Unary& node, SourcePos pos, const BindContext& context) {
  BoundPtr operand = bindExpr(*node.operand, context);
  switch (node.op) {
    case ast::UnaryOp::Negate:
      return std::make_unique<UnaryExpr>(negateValue, std::move(operand), pos);
    case ast::UnaryOp::Not:
      return std::make_unique<UnaryExpr>(logicalNot, std::move(operand), pos);
    case ast::UnaryOp::Plus:
      break;
  }
  auto plus = [](const Value& v) -> Value {
    if (!v.isNull() && !v.isNumeric()) throw Error(ErrorKind::Type, "unary plus cannot take " + v.typeLabel());
    return v;
  };
  return std::make_unique<UnaryExpr>(+plus, std::move(operand), pos);
}

BoundPtr bindBinary(const ast::Binary& node, SourcePos pos, const BindContext& context) {
  BoundPtr lhs = bindExpr(*node.lhs, context);
  BoundPtr rhs = bindExpr(*node.rhs, context);
  auto compare = [&](CompareOp op) { return std::make_unique<CompareExpr>(op, std::move(lhs), std::move(rhs), pos); };
  auto arithmetic = [&](ArithmeticExpr::Fn fn) {
    return std::make_unique<ArithmeticExpr>(fn, std::move(lhs), std::move(rhs), pos);
  };
  switch (node.op) {
    case ast::BinaryOp::Add: return arithmetic(addValues);
    case ast::BinaryOp::Sub: return arithmetic(subtractValues);
    case ast::BinaryOp::Mul: return arithmetic(multiplyValues);
    case ast::BinaryOp::Div: return arithmetic(divideValues);
    case ast::BinaryOp::Mod: return arithmetic(moduloValues);
    case ast::BinaryOp::Concat: return arithmetic(concatValues);
    case ast::BinaryOp::Eq: return compare(CompareOp::Eq);
    case ast::BinaryOp::Ne: return compare(CompareOp::Ne);
    case ast::BinaryOp::Lt: return compare(CompareOp::Lt);
    case ast::BinaryOp::Le: return compare(CompareOp::Le);
    case ast::BinaryOp::Gt: return compare(CompareOp::Gt);
    case ast::BinaryOp::Ge: return compare(CompareOp::Ge);
    case ast::BinaryOp::And: return std::make_unique<LogicExpr>(true, std::move(lhs), std::move(rhs), pos);
    case ast::BinaryOp::Or: return std::make_unique<LogicExpr>(false, std::move(lhs), std::move(rhs), pos);
  }
  throw Error(ErrorKind::Syntax, "unknown operator", pos);
}

BoundPtr bindCall(const ast::Call& call, SourcePos pos, const BindContext& context) {
  if (findAggregate(call.name)) {
    throw Error(ErrorKind::Syntax, "aggregate functions are not allowed in " + std::string(context.clause), pos);
  }
  const ScalarFunction* function = findScalarFunction(call.name);
  if (!function) throw Error(ErrorKind::Catalog, "no such function: " + call.name, pos);

  int count = static_cast<int>(call.args.size());
  if (call.star || count < function->minArgs || (function->maxArgs >= 0 && count > function->maxArgs)) {
    throw Error(ErrorKind::Syntax, "wrong number of arguments for " + call.name, pos);
  }
  std::vector<BoundPtr> args = bindAll(call.args, context);
  if (!function->call) return std::make_unique<CoalesceExpr>(std::move(args));
  return std::make_unique<CallExpr>(function, std::move(args), pos);
}

}  // namespace

BoundPtr makeSlotExpr(size_t slot) { return std::make_unique<SlotExpr>(slot); }

void Scope::addTable(const Table& table, const std::string& qualifier, int tablePosition) {
  for (const Column& column : table.columns()) {
    columns_.push_back(ScopeColumn{qualifier, column.name, tablePosition});
  }
}

Scope::Resolved Scope::resolve(const ast::ColumnRef& ref, SourcePos pos) const {
  std::optional<Resolved> found;
  for (size_t slot = 0; slot < columns_.size(); ++slot) {
    const ScopeColumn& column = columns_[slot];
    if (!equalsIgnoreCase(column.name, ref.name)) continue;
    if (!ref.qualifier.empty() && !equalsIgnoreCase(column.qualifier, ref.qualifier)) continue;
    if (found) throw Error(ErrorKind::Catalog, "ambiguous column reference: " + ref.name, pos);
    found = Resolved{slot, column.table};
  }
  if (found) return *found;
  if (!ref.qualifier.empty() && !hasQualifier(ref.qualifier)) {
    throw Error(ErrorKind::Catalog, "no such table or alias: " + ref.qualifier, pos);
  }
  std::string shown = ref.qualifier.empty() ? ref.name : ref.qualifier + "." + ref.name;
  throw Error(ErrorKind::Catalog, "no such column: " + shown, pos);
}

bool Scope::hasQualifier(const std::string& qualifier) const {
  return std::any_of(columns_.begin(), columns_.end(),
                     [&](const ScopeColumn& column) { return equalsIgnoreCase(column.qualifier, qualifier); });
}

BoundPtr bindExpr(const ast::Expr& expr, const BindContext& context) {
  if (context.grouped) {
    const auto& keys = context.grouped->keys;
    auto it = std::find(keys.begin(), keys.end(), canonicalExpr(expr, *context.grouped->source));
    if (it != keys.end()) return makeSlotExpr(static_cast<size_t>(it - keys.begin()));
  }

  SourcePos pos = expr.pos;
  return std::visit(
      overloaded{
          [&](const ast::Literal& n) -> BoundPtr { return std::make_unique<LiteralExpr>(n.value); },
          [&](const ast::ColumnRef& n) -> BoundPtr {
            if (context.grouped) {
              throw Error(ErrorKind::Syntax,
                          "column " + n.name + " must appear in GROUP BY or inside an aggregate function", pos);
            }
            if (!context.scope) {
              throw Error(ErrorKind::Syntax, "column references are not allowed in " + std::string(context.clause), pos);
            }
            return makeSlotExpr(context.scope->resolve(n, pos).slot);
          },
          [&](const ast::Unary& n) -> BoundPtr { return bindUnary(n, pos, context); },
          [&](const ast::Binary& n) -> BoundPtr { return bindBinary(n, pos, context); },
          [&](const ast::IsNull& n) -> BoundPtr {
            return std::make_unique<IsNullExpr>(bindExpr(*n.operand, context), n.negated);
          },
          [&](const ast::InList& n) -> BoundPtr {
            return std::make_unique<InExpr>(bindExpr(*n.operand, context), bindAll(n.items, context), n.negated, pos);
          },
          [&](const ast::Between& n) -> BoundPtr {
            return std::make_unique<BetweenExpr>(bindExpr(*n.operand, context), bindExpr(*n.low, context),
                                                 bindExpr(*n.high, context), n.negated, pos);
          },
          [&](const ast::Like& n) -> BoundPtr {
            return std::make_unique<LikeExpr>(bindExpr(*n.operand, context), bindExpr(*n.pattern, context), n.negated,
                                              pos);
          },
          [&](const ast::Cast& n) -> BoundPtr {
            return std::make_unique<CastExpr>(bindExpr(*n.operand, context), n.target, pos);
          },
          [&](const ast::Call& n) -> BoundPtr { return bindCall(n, pos, context); },
      },
      expr.node);
}

std::string canonicalExpr(const ast::Expr& expr, const Scope& scope) {
  ast::ColumnPrinter columns = [&scope](const ast::ColumnRef& ref, SourcePos pos) {
    return "#" + std::to_string(scope.resolve(ref, pos).slot);
  };
  return ast::printExpr(expr, &columns);
}

bool isAggregateCall(const ast::Expr& expr) {
  const auto* call = std::get_if<ast::Call>(&expr.node);
  return call && findAggregate(call->name).has_value();
}

bool containsAggregate(const ast::Expr& expr) {
  if (isAggregateCall(expr)) return true;
  bool found = false;
  ast::forEachChild(expr, [&](const ast::Expr& child) { found = found || containsAggregate(child); });
  return found;
}

void collectAggregates(const ast::Expr& expr, std::vector<const ast::Expr*>& out) {
  if (isAggregateCall(expr)) {
    out.push_back(&expr);
    return;
  }
  ast::forEachChild(expr, [&](const ast::Expr& child) { collectAggregates(child, out); });
}

Value evalConstant(const ast::Expr& expr, const char* clause) {
  BindContext context;
  context.clause = clause;
  return bindExpr(expr, context)->eval(Row{});
}

void Predicate::add(BoundPtr term, std::string text, SourcePos pos) {
  terms_.push_back(Term{std::move(term), pos});
  if (!text_.empty()) text_ += " AND ";
  text_ += text;
}

bool Predicate::test(const Row& row) const {
  for (const Term& term : terms_) {
    Value result = term.expr->eval(row);
    if (locating(term.pos, [&] { return truthOf(result); }) != true) return false;
  }
  return true;
}

}  // namespace minisql
