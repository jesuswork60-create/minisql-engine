#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "binder.h"
#include "functions.h"
#include "minisql/catalog.h"

namespace minisql {

// Pull-based physical operators. open() starts (or restarts) a run and next()
// yields one row at a time. The row passed to open() holds the current outer
// row when the operator is the inner side of a join, and is empty otherwise.
class Operator {
 public:
  virtual ~Operator() = default;
  virtual void open(const Row& outer) = 0;
  virtual bool next(Row& out) = 0;
  virtual std::string describe() const = 0;
  virtual std::vector<const Operator*> children() const { return {}; }
};
using OperatorPtr = std::unique_ptr<Operator>;

// An operator reading one base table. UPDATE and DELETE use rowId() to learn
// which stored row the last next() produced.
class TableAccess : public Operator {
 public:
  RowId rowId() const { return current_; }
  // True when open() needs the outer row, which makes a join above it indexed.
  virtual bool dependsOnOuter() const { return false; }

 protected:
  RowId current_ = 0;
};

// One line per operator, children indented under their parent.
std::vector<std::string> renderPlan(const Operator& root, int indent = 0);

// Pulls every remaining row.
std::vector<Row> drain(Operator& root);

class SingleRow : public Operator {
 public:
  void open(const Row&) override { done_ = false; }
  bool next(Row& out) override;
  std::string describe() const override { return "SingleRow"; }

 private:
  bool done_ = false;
};

class TableScan : public TableAccess {
 public:
  TableScan(const Table& table, std::string label, Predicate filter)
      : table_(table), label_(std::move(label)), filter_(std::move(filter)) {}
  void open(const Row&) override { position_ = 0; }
  bool next(Row& out) override;
  std::string describe() const override;

 private:
  const Table& table_;
  std::string label_;
  Predicate filter_;
  RowId position_ = 0;
};

class IndexScan : public TableAccess {
 public:
  IndexScan(const Table& table, std::string label, const HashIndex& index, BoundPtr key, std::string keyText,
            bool usesOuter, Predicate filter, SourcePos comparisonPos)
      : table_(table), label_(std::move(label)), index_(index), key_(std::move(key)), keyText_(std::move(keyText)),
        usesOuter_(usesOuter), filter_(std::move(filter)), comparisonPos_(comparisonPos) {}
  void open(const Row& outer) override;
  bool next(Row& out) override;
  std::string describe() const override;
  bool dependsOnOuter() const override { return usesOuter_; }

 private:
  const Table& table_;
  std::string label_;
  const HashIndex& index_;
  BoundPtr key_;
  std::string keyText_;
  bool usesOuter_;
  Predicate filter_;
  SourcePos comparisonPos_;
  std::vector<RowId> matches_;
  size_t position_ = 0;
};

class Filter : public Operator {
 public:
  Filter(OperatorPtr child, Predicate predicate) : child_(std::move(child)), predicate_(std::move(predicate)) {}
  void open(const Row& outer) override { child_->open(outer); }
  bool next(Row& out) override;
  std::string describe() const override { return "Filter (" + predicate_.text() + ")"; }
  std::vector<const Operator*> children() const override { return {child_.get()}; }

 private:
  OperatorPtr child_;
  Predicate predicate_;
};

// Left-deep join: restarts the inner table for every outer row. When the inner
// side is an IndexScan keyed on the outer row, each restart is a hash lookup.
class NestedLoopJoin : public Operator {
 public:
  NestedLoopJoin(OperatorPtr outer, std::unique_ptr<TableAccess> inner, Predicate condition)
      : outer_(std::move(outer)), inner_(std::move(inner)), condition_(std::move(condition)) {}
  void open(const Row& context) override;
  bool next(Row& out) override;
  std::string describe() const override;
  std::vector<const Operator*> children() const override { return {outer_.get(), inner_.get()}; }

 private:
  OperatorPtr outer_;
  std::unique_ptr<TableAccess> inner_;
  Predicate condition_;
  Row outerRow_;
  bool haveOuter_ = false;
};

struct AggregateSpec {
  AggregateKind kind;
  BoundPtr argument;  // null for COUNT(*)
  std::string text;
  SourcePos pos;
};

// Consumes its input on open(), then yields one row per group: key values
// followed by aggregate results. Groups come out in first-seen order. With no
// GROUP BY there is exactly one group, even over empty input.
class HashAggregate : public Operator {
 public:
  HashAggregate(OperatorPtr child, std::vector<BoundPtr> keys, std::vector<std::string> keyTexts,
                std::vector<AggregateSpec> aggregates)
      : child_(std::move(child)), keys_(std::move(keys)), keyTexts_(std::move(keyTexts)),
        aggregates_(std::move(aggregates)) {}
  void open(const Row& outer) override;
  bool next(Row& out) override;
  std::string describe() const override;
  std::vector<const Operator*> children() const override { return {child_.get()}; }

 private:
  OperatorPtr child_;
  std::vector<BoundPtr> keys_;
  std::vector<std::string> keyTexts_;
  std::vector<AggregateSpec> aggregates_;
  std::vector<Row> output_;
  size_t position_ = 0;
};

struct SortKey {
  BoundPtr expr;
  bool descending;
  std::string text;
};

// Stable. NULL sorts before every value when ascending, after when descending.
class Sort : public Operator {
 public:
  Sort(OperatorPtr child, std::vector<SortKey> keys) : child_(std::move(child)), keys_(std::move(keys)) {}
  void open(const Row& outer) override;
  bool next(Row& out) override;
  std::string describe() const override;
  std::vector<const Operator*> children() const override { return {child_.get()}; }

 private:
  OperatorPtr child_;
  std::vector<SortKey> keys_;
  std::vector<Row> rows_;
  size_t position_ = 0;
};

class Limit : public Operator {
 public:
  Limit(OperatorPtr child, std::optional<int64_t> limit, int64_t offset)
      : child_(std::move(child)), limit_(limit), offset_(offset) {}
  void open(const Row& outer) override;
  bool next(Row& out) override;
  std::string describe() const override;
  std::vector<const Operator*> children() const override { return {child_.get()}; }

 private:
  OperatorPtr child_;
  std::optional<int64_t> limit_;
  int64_t offset_;
  int64_t produced_ = 0;
  bool skipped_ = false;
};

class Project : public Operator {
 public:
  Project(OperatorPtr child, std::vector<BoundPtr> exprs, std::vector<std::string> names)
      : child_(std::move(child)), exprs_(std::move(exprs)), names_(std::move(names)) {}
  void open(const Row& outer) override { child_->open(outer); }
  bool next(Row& out) override;
  std::string describe() const override;
  std::vector<const Operator*> children() const override { return {child_.get()}; }

 private:
  OperatorPtr child_;
  std::vector<BoundPtr> exprs_;
  std::vector<std::string> names_;
  Row input_;
};

}  // namespace minisql
