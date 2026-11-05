#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "minisql/value.h"

namespace minisql {

struct ScalarFunction {
  std::string_view name;
  int minArgs;
  int maxArgs;  // -1 for no upper limit
  // Null for functions the binder compiles itself because they do not take
  // all their arguments up front (COALESCE stops at the first non-NULL one).
  Value (*call)(const std::vector<Value>& args);
};

// Name must be upper case. nullptr when there is no such function.
const ScalarFunction* findScalarFunction(std::string_view name);

enum class AggregateKind { Count, Sum, Avg, Min, Max };

std::optional<AggregateKind> findAggregate(std::string_view name);

// Running state of one aggregate over one group. NULL inputs are skipped.
class AggregateState {
 public:
  explicit AggregateState(AggregateKind kind) : kind_(kind) {}

  void addRow() { ++count_; }  // COUNT(*)
  void add(const Value& value);
  Value result() const;

 private:
  AggregateKind kind_;
  int64_t count_ = 0;
  double floatSum_ = 0;  // AVG
  Value accumulated_;    // SUM, MIN, MAX
};

}  // namespace minisql
