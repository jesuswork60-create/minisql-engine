#include "functions.h"

#include <cmath>
#include <limits>

#include "minisql/error.h"

namespace minisql {

namespace {

const std::string& textArg(const char* function, const Value& v) {
  if (!v.isText()) throw Error(ErrorKind::Type, std::string(function) + " expects TEXT, got " + v.typeLabel());
  return v.asText();
}

Value callUpper(const std::vector<Value>& args) {
  if (args[0].isNull()) return Value::Null();
  return Value::Text(toUpperAscii(textArg("UPPER", args[0])));
}

Value callLower(const std::vector<Value>& args) {
  if (args[0].isNull()) return Value::Null();
  return Value::Text(toLowerAscii(textArg("LOWER", args[0])));
}

Value callLength(const std::vector<Value>& args) {
  if (args[0].isNull()) return Value::Null();
  return Value::Int(static_cast<int64_t>(textArg("LENGTH", args[0]).size()));
}

Value callAbs(const std::vector<Value>& args) {
  const Value& v = args[0];
  if (v.isNull()) return Value::Null();
  if (v.isFloat()) return Value::Float(std::fabs(v.asFloat()));
  if (!v.isInt()) throw Error(ErrorKind::Type, "ABS expects a number, got " + v.typeLabel());
  if (v.asInt() == std::numeric_limits<int64_t>::min()) {
    throw Error(ErrorKind::Arithmetic, "INT result out of range");
  }
  return Value::Int(v.asInt() < 0 ? -v.asInt() : v.asInt());
}

constexpr ScalarFunction kScalarFunctions[] = {
    {"ABS", 1, 1, callAbs},
    {"COALESCE", 1, -1, nullptr},
    {"LENGTH", 1, 1, callLength},
    {"LOWER", 1, 1, callLower},
    {"UPPER", 1, 1, callUpper},
};

}  // namespace

const ScalarFunction* findScalarFunction(std::string_view name) {
  for (const ScalarFunction& function : kScalarFunctions) {
    if (function.name == name) return &function;
  }
  return nullptr;
}

std::optional<AggregateKind> findAggregate(std::string_view name) {
  if (name == "COUNT") return AggregateKind::Count;
  if (name == "SUM") return AggregateKind::Sum;
  if (name == "AVG") return AggregateKind::Avg;
  if (name == "MIN") return AggregateKind::Min;
  if (name == "MAX") return AggregateKind::Max;
  return std::nullopt;
}

void AggregateState::add(const Value& value) {
  if (value.isNull()) return;
  ++count_;
  switch (kind_) {
    case AggregateKind::Count:
      break;
    case AggregateKind::Sum:
      if (!value.isNumeric()) throw Error(ErrorKind::Type, "SUM expects a number, got " + value.typeLabel());
      accumulated_ = accumulated_.isNull() ? value : addValues(accumulated_, value);
      break;
    case AggregateKind::Avg:
      if (!value.isNumeric()) throw Error(ErrorKind::Type, "AVG expects a number, got " + value.typeLabel());
      floatSum_ += value.toDouble();
      break;
    case AggregateKind::Min:
      if (accumulated_.isNull() || compareValues(value, accumulated_) < 0) accumulated_ = value;
      break;
    case AggregateKind::Max:
      if (accumulated_.isNull() || compareValues(value, accumulated_) > 0) accumulated_ = value;
      break;
  }
}

Value AggregateState::result() const {
  switch (kind_) {
    case AggregateKind::Count:
      return Value::Int(count_);
    case AggregateKind::Avg:
      return count_ == 0 ? Value::Null() : Value::Float(floatSum_ / static_cast<double>(count_));
    default:
      return accumulated_;
  }
}

}  // namespace minisql
