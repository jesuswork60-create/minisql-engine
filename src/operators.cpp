#include "operators.h"

#include <algorithm>
#include <numeric>
#include <unordered_map>

namespace minisql {

namespace {

std::string joinTexts(const std::vector<std::string>& parts) {
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) out += ", ";
    out += parts[i];
  }
  return out;
}

}  // namespace

std::vector<std::string> renderPlan(const Operator& root, int indent) {
  std::vector<std::string> lines;
  lines.push_back(std::string(static_cast<size_t>(indent) * 2, ' ') + root.describe());
  for (const Operator* child : root.children()) {
    auto nested = renderPlan(*child, indent + 1);
    lines.insert(lines.end(), nested.begin(), nested.end());
  }
  return lines;
}

std::vector<Row> drain(Operator& root) {
  std::vector<Row> rows;
  Row row;
  while (root.next(row)) rows.push_back(row);
  return rows;
}

bool SingleRow::next(Row& out) {
  if (done_) return false;
  done_ = true;
  out.clear();
  return true;
}

bool TableScan::next(Row& out) {
  while (position_ < table_.slotCount()) {
    RowId id = position_++;
    if (!table_.isLive(id)) continue;
    const Row& row = table_.row(id);
    if (!filter_.empty() && !filter_.test(row)) continue;
    current_ = id;
    out = row;
    return true;
  }
  return false;
}

std::string TableScan::describe() const {
  std::string text = "SeqScan " + label_;
  if (!filter_.empty()) text += " filter=(" + filter_.text() + ")";
  return text;
}

void IndexScan::open(const Row& outer) {
  matches_.clear();
  position_ = 0;
  Value key = key_->eval(outer);
  if (key.isNull()) return;  // = NULL is never true
  locating(comparisonPos_, [&] { requireComparable(key, table_.columns()[index_.column()].type); });
  if (const auto* ids = index_.lookup(key)) {
    matches_ = *ids;
    // Same row order a sequential scan would give.
    std::sort(matches_.begin(), matches_.end());
  }
}

bool IndexScan::next(Row& out) {
  while (position_ < matches_.size()) {
    RowId id = matches_[position_++];
    const Row& row = table_.row(id);
    if (!filter_.empty() && !filter_.test(row)) continue;
    current_ = id;
    out = row;
    return true;
  }
  return false;
}

std::string IndexScan::describe() const {
  std::string text = "IndexScan " + label_ + " using " + index_.name() + " key=(" + keyText_ + ")";
  if (!filter_.empty()) text += " filter=(" + filter_.text() + ")";
  return text;
}

bool Filter::next(Row& out) {
  while (child_->next(out)) {
    if (predicate_.test(out)) return true;
  }
  return false;
}

void NestedLoopJoin::open(const Row& context) {
  outer_->open(context);
  haveOuter_ = false;
}

bool NestedLoopJoin::next(Row& out) {
  Row innerRow;
  for (;;) {
    if (!haveOuter_) {
      if (!outer_->next(outerRow_)) return false;
      inner_->open(outerRow_);
      haveOuter_ = true;
    }
    while (inner_->next(innerRow)) {
      out = outerRow_;
      out.insert(out.end(), innerRow.begin(), innerRow.end());
      if (condition_.empty() || condition_.test(out)) return true;
    }
    haveOuter_ = false;
  }
}

std::string NestedLoopJoin::describe() const {
  std::string text = inner_->dependsOnOuter() ? "IndexNestedLoopJoin" : "NestedLoopJoin";
  if (!condition_.empty()) text += " on=(" + condition_.text() + ")";
  return text;
}

void HashAggregate::open(const Row& outer) {
  output_.clear();
  position_ = 0;
  child_->open(outer);

  std::vector<std::vector<AggregateState>> states;
  std::vector<Row> groupKeys;
  std::unordered_map<Row, size_t, RowHash, RowKeyEq> groupIndex;

  auto newGroup = [&](Row key) {
    std::vector<AggregateState> fresh;
    fresh.reserve(aggregates_.size());
    for (const AggregateSpec& spec : aggregates_) fresh.emplace_back(spec.kind);
    states.push_back(std::move(fresh));
    groupKeys.push_back(std::move(key));
    return states.size() - 1;
  };

  if (keys_.empty()) newGroup(Row{});

  Row row;
  Row key;
  while (child_->next(row)) {
    size_t group = 0;
    if (!keys_.empty()) {
      key.clear();
      for (const BoundPtr& expr : keys_) key.push_back(expr->eval(row));
      auto found = groupIndex.find(key);
      if (found == groupIndex.end()) {
        group = newGroup(key);
        groupIndex.emplace(key, group);
      } else {
        group = found->second;
      }
    }
    for (size_t i = 0; i < aggregates_.size(); ++i) {
      const AggregateSpec& spec = aggregates_[i];
      if (spec.argument) {
        Value input = spec.argument->eval(row);
        locating(spec.pos, [&] {
          states[group][i].add(input);
          return 0;
        });
      } else {
        states[group][i].addRow();
      }
    }
  }

  output_.reserve(states.size());
  for (size_t group = 0; group < states.size(); ++group) {
    Row result = std::move(groupKeys[group]);
    for (const AggregateState& state : states[group]) result.push_back(state.result());
    output_.push_back(std::move(result));
  }
}

bool HashAggregate::next(Row& out) {
  if (position_ >= output_.size()) return false;
  out = output_[position_++];
  return true;
}

std::string HashAggregate::describe() const {
  std::vector<std::string> aggregateTexts;
  aggregateTexts.reserve(aggregates_.size());
  for (const AggregateSpec& spec : aggregates_) aggregateTexts.push_back(spec.text);
  return "HashAggregate keys=[" + joinTexts(keyTexts_) + "] aggregates=[" + joinTexts(aggregateTexts) + "]";
}

void Sort::open(const Row& outer) {
  child_->open(outer);
  position_ = 0;
  std::vector<Row> input = drain(*child_);

  // Keys are computed once up front; the comparator only reads them.
  std::vector<std::vector<Value>> sortValues;
  sortValues.reserve(input.size());
  for (const Row& row : input) {
    std::vector<Value> values;
    values.reserve(keys_.size());
    for (const SortKey& key : keys_) values.push_back(key.expr->eval(row));
    sortValues.push_back(std::move(values));
  }

  std::vector<size_t> order(input.size());
  std::iota(order.begin(), order.end(), size_t{0});
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    for (size_t k = 0; k < keys_.size(); ++k) {
      const Value& x = sortValues[a][k];
      const Value& y = sortValues[b][k];
      int cmp;
      if (x.isNull() || y.isNull()) {
        cmp = static_cast<int>(y.isNull()) - static_cast<int>(x.isNull());
      } else {
        cmp = compareValues(x, y);
      }
      if (cmp != 0) return keys_[k].descending ? cmp > 0 : cmp < 0;
    }
    return false;
  });

  rows_.clear();
  rows_.reserve(input.size());
  for (size_t index : order) rows_.push_back(std::move(input[index]));
}

bool Sort::next(Row& out) {
  if (position_ >= rows_.size()) return false;
  out = rows_[position_++];
  return true;
}

std::string Sort::describe() const {
  std::vector<std::string> parts;
  parts.reserve(keys_.size());
  for (const SortKey& key : keys_) parts.push_back(key.text + (key.descending ? " DESC" : " ASC"));
  return "Sort [" + joinTexts(parts) + "]";
}

void Limit::open(const Row& outer) {
  child_->open(outer);
  produced_ = 0;
  skipped_ = false;
}

bool Limit::next(Row& out) {
  if (limit_ && produced_ >= *limit_) return false;
  if (!skipped_) {
    skipped_ = true;
    for (int64_t i = 0; i < offset_; ++i) {
      if (!child_->next(out)) return false;
    }
  }
  if (!child_->next(out)) return false;
  ++produced_;
  return true;
}

std::string Limit::describe() const {
  std::string text = "Limit ";
  text += limit_ ? std::to_string(*limit_) : "all";
  if (offset_ > 0) text += " offset " + std::to_string(offset_);
  return text;
}

bool Project::next(Row& out) {
  if (!child_->next(input_)) return false;
  out.clear();
  out.reserve(exprs_.size());
  for (const BoundPtr& expr : exprs_) out.push_back(expr->eval(input_));
  return true;
}

std::string Project::describe() const { return "Project [" + joinTexts(names_) + "]"; }

}  // namespace minisql
