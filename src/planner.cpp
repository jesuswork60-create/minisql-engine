#include "planner.h"

#include <algorithm>
#include <utility>

#include "functions.h"

namespace minisql {

namespace {

void splitConjuncts(const ast::Expr& expr, std::vector<const ast::Expr*>& out) {
  if (const auto* binary = std::get_if<ast::Binary>(&expr.node)) {
    if (binary->op == ast::BinaryOp::And) {
      splitConjuncts(*binary->lhs, out);
      splitConjuncts(*binary->rhs, out);
      return;
    }
  }
  out.push_back(&expr);
}

void collectTables(const ast::Expr& expr, const Scope& scope, std::vector<int>& tables) {
  if (const auto* ref = std::get_if<ast::ColumnRef>(&expr.node)) {
    tables.push_back(scope.resolve(*ref, expr.pos).table);
  }
  ast::forEachChild(expr, [&](const ast::Expr& child) { collectTables(child, scope, tables); });
}

struct FromTable {
  const Table* table;
  std::string qualifier;
  std::string label;  // "users" or "users AS u", for EXPLAIN
  Scope own;          // only this table; slots are relative to its rows
};

// A WHERE or ON term, placed at the first join step where its columns exist.
struct Conjunct {
  const ast::Expr* expr;
  const Scope* visible;   // scope the term was written against
  std::vector<int> tables;
  int step;  // the highest table it references
  bool consumed = false;
};

struct IndexChoice {
  const HashIndex* index;
  const ast::Expr* key;
  Conjunct* conjunct;
};

// Matches column = key or key = column, where column is an indexed column of
// the table at `step` and key only reads tables joined before it.
std::optional<IndexChoice> matchIndexedEquality(Conjunct& conjunct, const Table& table, int step) {
  const auto* binary = std::get_if<ast::Binary>(&conjunct.expr->node);
  if (!binary || binary->op != ast::BinaryOp::Eq) return std::nullopt;

  auto attempt = [&](const ast::Expr& side, const ast::Expr& other) -> std::optional<IndexChoice> {
    const auto* ref = std::get_if<ast::ColumnRef>(&side.node);
    if (!ref) return std::nullopt;
    Scope::Resolved resolved = conjunct.visible->resolve(*ref, side.pos);
    if (resolved.table != step) return std::nullopt;
    std::vector<int> keyTables;
    collectTables(other, *conjunct.visible, keyTables);
    bool earlierOnly = std::all_of(keyTables.begin(), keyTables.end(), [&](int t) { return t < step; });
    if (!earlierOnly) return std::nullopt;
    auto column = table.columnIndex(ref->name);
    const HashIndex* index = column ? table.indexOn(*column) : nullptr;
    if (!index) return std::nullopt;
    return IndexChoice{index, &other, &conjunct};
  };

  if (auto choice = attempt(*binary->lhs, *binary->rhs)) return choice;
  return attempt(*binary->rhs, *binary->lhs);
}

// Prefers a unique index, which can match at most one row.
std::optional<IndexChoice> chooseIndex(std::vector<Conjunct*>& candidates, const Table& table, int step) {
  std::optional<IndexChoice> best;
  for (Conjunct* conjunct : candidates) {
    auto choice = matchIndexedEquality(*conjunct, table, step);
    if (choice && (!best || (choice->index->unique() && !best->index->unique()))) best = choice;
  }
  return best;
}

Predicate buildPredicate(const std::vector<const ast::Expr*>& terms, const Scope& scope, const char* clause) {
  Predicate predicate;
  BindContext context;
  context.scope = &scope;
  context.clause = clause;
  for (const ast::Expr* term : terms) predicate.add(bindExpr(*term, context), ast::printExpr(*term), term->pos);
  return predicate;
}

std::unique_ptr<TableAccess> makeAccess(const Table& table, const std::string& label, const Scope& own,
                                        const std::optional<IndexChoice>& choice, const Scope* keyScope,
                                        const std::vector<const ast::Expr*>& filterTerms) {
  Predicate filter = buildPredicate(filterTerms, own, "WHERE");
  if (!choice) return std::make_unique<TableScan>(table, label, std::move(filter));

  std::vector<int> keyTables;
  if (keyScope) collectTables(*choice->key, *keyScope, keyTables);
  BindContext keyContext;
  keyContext.scope = keyScope;
  keyContext.clause = "WHERE";
  return std::make_unique<IndexScan>(table, label, *choice->index, bindExpr(*choice->key, keyContext),
                                     ast::printExpr(*choice->key), !keyTables.empty(), std::move(filter),
                                     choice->conjunct->expr->pos);
}

class SelectPlanner {
 public:
  SelectPlanner(const ast::Select& select, const Catalog& catalog) : select_(select), catalog_(catalog) {}

  SelectPlan plan() {
    loadTables();
    OperatorPtr root = buildSource();
    bool aggregated = isAggregated();

    expandOutputs(aggregated);
    GroupedOutput grouped;
    if (aggregated) {
      root = buildAggregate(std::move(root), grouped);
    }
    BindContext outputContext;
    outputContext.scope = &fullScope();
    outputContext.clause = "the SELECT list";
    outputContext.grouped = aggregated ? &grouped : nullptr;

    if (select_.having) {
      BindContext havingContext = outputContext;
      havingContext.clause = "HAVING";
      Predicate predicate;
      predicate.add(bindExpr(*select_.having, havingContext), ast::printExpr(*select_.having), select_.having->pos);
      root = std::make_unique<Filter>(std::move(root), std::move(predicate));
    }
    if (!select_.orderBy.empty()) root = buildSort(std::move(root), outputContext);
    root = buildLimit(std::move(root));

    std::vector<BoundPtr> exprs;
    std::vector<std::string> names;
    for (const Output& output : outputs_) {
      exprs.push_back(output.expr ? bindExpr(*output.expr, outputContext) : makeSlotExpr(output.slot));
      names.push_back(output.name);
    }
    SelectPlan plan;
    plan.columns = names;
    plan.root = std::make_unique<Project>(std::move(root), std::move(exprs), std::move(names));
    return plan;
  }

 private:
  struct Output {
    std::string name;
    const ast::Expr* expr;  // null when it came from expanding * (use slot)
    size_t slot;
    std::string alias;
  };

  const Scope& fullScope() const {
    static const Scope kEmpty;
    return prefixScopes_.empty() ? kEmpty : prefixScopes_.back();
  }

  void addTable(const ast::TableRef& ref) {
    const Table& table = catalog_.require(ref.name, ref.pos);
    std::string qualifier = ref.alias.empty() ? table.name() : ref.alias;
    for (const FromTable& existing : tables_) {
      if (equalsIgnoreCase(existing.qualifier, qualifier)) {
        throw Error(ErrorKind::Catalog, "table name " + qualifier + " is used twice; give one an alias", ref.pos);
      }
    }
    int position = static_cast<int>(tables_.size());
    FromTable from{&table, qualifier, table.name(), Scope{}};
    if (!ref.alias.empty()) from.label += " AS " + ref.alias;
    from.own.addTable(table, qualifier, position);

    Scope prefix = prefixScopes_.empty() ? Scope{} : prefixScopes_.back();
    prefix.addTable(table, qualifier, position);
    prefixScopes_.push_back(std::move(prefix));
    tables_.push_back(std::move(from));
  }

  void loadTables() {
    if (!select_.from) return;
    addTable(*select_.from);
    for (const ast::Join& join : select_.joins) addTable(join.table);
  }

  void addConjuncts(const ast::Expr& expr, const Scope& visible, std::vector<Conjunct>& out) const {
    std::vector<const ast::Expr*> terms;
    splitConjuncts(expr, terms);
    for (const ast::Expr* term : terms) {
      Conjunct conjunct{term, &visible, {}, 0};
      collectTables(*term, visible, conjunct.tables);
      for (int table : conjunct.tables) conjunct.step = std::max(conjunct.step, table);
      out.push_back(std::move(conjunct));
    }
  }

  OperatorPtr buildSource() {
    if (tables_.empty()) {
      OperatorPtr root = std::make_unique<SingleRow>();
      if (select_.where) {
        static const Scope kEmpty;
        std::vector<const ast::Expr*> terms;
        splitConjuncts(*select_.where, terms);
        root = std::make_unique<Filter>(std::move(root), buildPredicate(terms, kEmpty, "WHERE"));
      }
      return root;
    }

    std::vector<Conjunct> conjuncts;
    // An ON clause only sees the tables joined so far.
    for (size_t i = 0; i < select_.joins.size(); ++i) {
      if (select_.joins[i].on) addConjuncts(*select_.joins[i].on, prefixScopes_[i + 1], conjuncts);
    }
    if (select_.where) addConjuncts(*select_.where, fullScope(), conjuncts);

    OperatorPtr root;
    for (size_t i = 0; i < tables_.size(); ++i) root = buildJoinStep(std::move(root), conjuncts, static_cast<int>(i));
    return root;
  }

  OperatorPtr buildJoinStep(OperatorPtr outer, std::vector<Conjunct>& conjuncts, int step) {
    const FromTable& from = tables_[static_cast<size_t>(step)];
    std::vector<Conjunct*> here;
    for (Conjunct& conjunct : conjuncts) {
      if (conjunct.step == step) here.push_back(&conjunct);
    }

    auto choice = chooseIndex(here, *from.table, step);
    if (choice) choice->conjunct->consumed = true;

    // Terms reading only this table filter inside the scan, before any row is
    // copied; the rest wait until the joined row exists.
    std::vector<const ast::Expr*> local;
    std::vector<const ast::Expr*> joined;
    for (Conjunct* conjunct : here) {
      if (conjunct->consumed) continue;
      bool ownOnly = std::all_of(conjunct->tables.begin(), conjunct->tables.end(), [&](int t) { return t == step; });
      (ownOnly ? local : joined).push_back(conjunct->expr);
    }

    const Scope* keyScope = step > 0 ? &prefixScopes_[static_cast<size_t>(step) - 1] : nullptr;
    auto access = makeAccess(*from.table, from.label, from.own, choice, keyScope, local);
    if (!outer) return access;
    Predicate condition = buildPredicate(joined, prefixScopes_[static_cast<size_t>(step)], "JOIN");
    return std::make_unique<NestedLoopJoin>(std::move(outer), std::move(access), std::move(condition));
  }

  bool isAggregated() const {
    if (!select_.groupBy.empty() || select_.having) return true;
    for (const ast::SelectItem& item : select_.items) {
      if (item.expr && containsAggregate(*item.expr)) return true;
    }
    for (const ast::OrderTerm& term : select_.orderBy) {
      if (containsAggregate(*term.expr)) return true;
    }
    return false;
  }

  void expandOutputs(bool aggregated) {
    for (const ast::SelectItem& item : select_.items) {
      if (item.expr) {
        std::string name = !item.alias.empty() ? item.alias : defaultColumnName(*item.expr);
        outputs_.push_back(Output{std::move(name), item.expr.get(), 0, item.alias});
        continue;
      }
      if (aggregated) throw Error(ErrorKind::Syntax, "* cannot be combined with GROUP BY or aggregates", item.pos);
      if (tables_.empty()) throw Error(ErrorKind::Syntax, "* needs a FROM clause", item.pos);
      if (!item.starTable.empty() && !fullScope().hasQualifier(item.starTable)) {
        throw Error(ErrorKind::Catalog, "no such table or alias: " + item.starTable, item.pos);
      }
      const auto& columns = fullScope().columns();
      for (size_t slot = 0; slot < columns.size(); ++slot) {
        if (item.starTable.empty() || equalsIgnoreCase(columns[slot].qualifier, item.starTable)) {
          outputs_.push_back(Output{columns[slot].name, nullptr, slot, ""});
        }
      }
    }
  }

  static std::string defaultColumnName(const ast::Expr& expr) {
    if (const auto* ref = std::get_if<ast::ColumnRef>(&expr.node)) return ref->name;
    return ast::printExpr(expr);
  }

  // ORDER BY may name an output by alias or by 1-based position; otherwise the
  // term is an ordinary expression over the rows being sorted.
  const Output* orderTarget(const ast::Expr& expr) const {
    if (const auto* literal = std::get_if<ast::Literal>(&expr.node)) {
      if (!literal->value.isInt()) return nullptr;
      int64_t ordinal = literal->value.asInt();
      if (ordinal < 1 || static_cast<size_t>(ordinal) > outputs_.size()) {
        throw Error(ErrorKind::Syntax, "ORDER BY position " + std::to_string(ordinal) + " is out of range", expr.pos);
      }
      return &outputs_[static_cast<size_t>(ordinal) - 1];
    }
    if (const auto* ref = std::get_if<ast::ColumnRef>(&expr.node)) {
      if (!ref->qualifier.empty()) return nullptr;
      for (const Output& output : outputs_) {
        if (!output.alias.empty() && equalsIgnoreCase(output.alias, ref->name)) return &output;
      }
    }
    return nullptr;
  }

  OperatorPtr buildAggregate(OperatorPtr source, GroupedOutput& grouped) {
    const Scope& scope = fullScope();
    grouped.source = &scope;

    BindContext keyContext;
    keyContext.scope = &scope;
    keyContext.clause = "GROUP BY";
    std::vector<BoundPtr> keys;
    std::vector<std::string> keyTexts;
    for (const ast::ExprPtr& expr : select_.groupBy) {
      keys.push_back(bindExpr(*expr, keyContext));
      keyTexts.push_back(ast::printExpr(*expr));
      grouped.keys.push_back(canonicalExpr(*expr, scope));
    }

    std::vector<const ast::Expr*> calls;
    for (const Output& output : outputs_) {
      if (output.expr) collectAggregates(*output.expr, calls);
    }
    if (select_.having) collectAggregates(*select_.having, calls);
    for (const ast::OrderTerm& term : select_.orderBy) {
      if (!orderTarget(*term.expr)) collectAggregates(*term.expr, calls);
    }

    std::vector<AggregateSpec> aggregates;
    for (const ast::Expr* call : calls) {
      std::string canonical = canonicalExpr(*call, scope);
      if (std::find(grouped.keys.begin(), grouped.keys.end(), canonical) != grouped.keys.end()) continue;
      aggregates.push_back(bindAggregate(*call, scope));
      grouped.keys.push_back(std::move(canonical));
    }
    return std::make_unique<HashAggregate>(std::move(source), std::move(keys), std::move(keyTexts),
                                           std::move(aggregates));
  }

  static AggregateSpec bindAggregate(const ast::Expr& expr, const Scope& scope) {
    const auto& call = std::get<ast::Call>(expr.node);
    if (call.star != (call.name == "COUNT" && call.args.empty()) || (!call.star && call.args.size() != 1)) {
      throw Error(ErrorKind::Syntax, "wrong number of arguments for " + call.name, expr.pos);
    }
    BoundPtr argument;
    if (!call.star) {
      BindContext context;
      context.scope = &scope;
      context.clause = "aggregate arguments";
      argument = bindExpr(*call.args[0], context);
    }
    return AggregateSpec{*findAggregate(call.name), std::move(argument), ast::printExpr(expr), expr.pos};
  }

  OperatorPtr buildSort(OperatorPtr child, const BindContext& context) {
    std::vector<SortKey> keys;
    for (const ast::OrderTerm& term : select_.orderBy) {
      const Output* target = orderTarget(*term.expr);
      BoundPtr bound;
      std::string text;
      if (target && !target->expr) {
        bound = makeSlotExpr(target->slot);
        text = target->name;
      } else {
        const ast::Expr& expr = target ? *target->expr : *term.expr;
        bound = bindExpr(expr, context);
        text = ast::printExpr(expr);
      }
      keys.push_back(SortKey{std::move(bound), term.descending, std::move(text)});
    }
    return std::make_unique<Sort>(std::move(child), std::move(keys));
  }

  static int64_t nonNegativeConstant(const ast::Expr& expr, const char* clause) {
    Value value = evalConstant(expr, clause);
    if (!value.isInt() || value.asInt() < 0) {
      throw Error(ErrorKind::Type, std::string(clause) + " must be a non-negative INT, got " + value.toSql(), expr.pos);
    }
    return value.asInt();
  }

  OperatorPtr buildLimit(OperatorPtr child) const {
    if (!select_.limit && !select_.offset) return child;
    std::optional<int64_t> limit;
    if (select_.limit) limit = nonNegativeConstant(*select_.limit, "LIMIT");
    int64_t offset = select_.offset ? nonNegativeConstant(*select_.offset, "OFFSET") : 0;
    return std::make_unique<Limit>(std::move(child), limit, offset);
  }

  const ast::Select& select_;
  const Catalog& catalog_;
  std::vector<FromTable> tables_;
  std::vector<Scope> prefixScopes_;  // [i] holds tables 0..i; rows joined so far use these slots
  std::vector<Output> outputs_;
};

}  // namespace

SelectPlan planSelect(const ast::Select& select, const Catalog& catalog) { return SelectPlanner(select, catalog).plan(); }

std::unique_ptr<TableAccess> planTableAccess(const Table& table, const ast::Expr* where) {
  Scope scope;
  scope.addTable(table, table.name(), 0);

  std::vector<const ast::Expr*> terms;
  if (where) splitConjuncts(*where, terms);

  std::vector<Conjunct> conjuncts;
  for (const ast::Expr* term : terms) {
    Conjunct conjunct{term, &scope, {}, 0};
    collectTables(*term, scope, conjunct.tables);
    conjuncts.push_back(std::move(conjunct));
  }
  std::vector<Conjunct*> candidates;
  for (Conjunct& conjunct : conjuncts) candidates.push_back(&conjunct);

  auto choice = chooseIndex(candidates, table, 0);
  std::vector<const ast::Expr*> filterTerms;
  for (const Conjunct& conjunct : conjuncts) {
    if (!choice || choice->conjunct != &conjunct) filterTerms.push_back(conjunct.expr);
  }
  return makeAccess(table, table.name(), scope, choice, nullptr, filterTerms);
}

}  // namespace minisql
