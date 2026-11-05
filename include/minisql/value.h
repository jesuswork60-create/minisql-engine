#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace minisql {

enum class Type { Int, Float, Text, Bool };

std::string_view typeName(Type type);
// Accepts the SQL spellings (INT, INTEGER, REAL, VARCHAR, ...), any case.
std::optional<Type> typeFromName(std::string_view name);

// A single SQL value. The default-constructed Value is NULL.
class Value {
 public:
  Value() = default;

  static Value Null() { return Value(); }
  static Value Int(int64_t v) { return Value(Data(v)); }
  static Value Float(double v) { return Value(Data(v)); }
  static Value Text(std::string v) { return Value(Data(std::move(v))); }
  static Value Bool(bool v) { return Value(Data(v)); }

  bool isNull() const { return std::holds_alternative<std::monostate>(data_); }
  bool isInt() const { return std::holds_alternative<int64_t>(data_); }
  bool isFloat() const { return std::holds_alternative<double>(data_); }
  bool isText() const { return std::holds_alternative<std::string>(data_); }
  bool isBool() const { return std::holds_alternative<bool>(data_); }
  bool isNumeric() const { return isInt() || isFloat(); }

  int64_t asInt() const { return std::get<int64_t>(data_); }
  double asFloat() const { return std::get<double>(data_); }
  const std::string& asText() const { return std::get<std::string>(data_); }
  bool asBool() const { return std::get<bool>(data_); }
  // Numeric value widened to double. Only valid when isNumeric().
  double toDouble() const { return isInt() ? static_cast<double>(asInt()) : asFloat(); }

  // nullopt for NULL.
  std::optional<Type> type() const;
  // Name for messages: the type name, or "NULL".
  std::string typeLabel() const;

  // Plain display form: NULL, 42, 2.5, hello, true.
  std::string toString() const;
  // SQL literal that parses back to this value: NULL, 42, 2.5, 'it''s', TRUE.
  std::string toSql() const;

 private:
  using Data = std::variant<std::monostate, int64_t, double, std::string, bool>;
  explicit Value(Data data) : data_(std::move(data)) {}
  Data data_;
};

using Row = std::vector<Value>;

// Key semantics, used by hash indexes and GROUP BY: NULL equals NULL, and
// numbers compare by value across INT and FLOAT, so 1 and 1.0 are one key.
bool sameKey(const Value& a, const Value& b);
size_t hashValue(const Value& v);

struct ValueHash {
  size_t operator()(const Value& v) const { return hashValue(v); }
};
struct ValueKeyEq {
  bool operator()(const Value& a, const Value& b) const { return sameKey(a, b); }
};
struct RowHash {
  size_t operator()(const Row& row) const;
};
struct RowKeyEq {
  bool operator()(const Row& a, const Row& b) const;
};

// Total order on two non-NULL values of comparable types: numbers with numbers
// (exact, even for large INT against FLOAT), text with text, bool with bool.
// Throws a type error for any other pairing.
int compareValues(const Value& a, const Value& b);
// Throws the same type error compareValues would for key against a column.
void requireComparable(const Value& key, Type columnType);

// Three-valued logic. A truth value is BOOL or NULL; anything else is a type error.
std::optional<bool> truthOf(const Value& v);
Value fromTruth(std::optional<bool> truth);
Value logicalNot(const Value& v);

// SQL comparison returning TRUE, FALSE or NULL (when either side is NULL).
enum class CompareOp { Eq, Ne, Lt, Le, Gt, Ge };
Value compareSql(CompareOp op, const Value& a, const Value& b);

// Arithmetic propagates NULL, promotes INT to FLOAT when mixed, and raises an
// arithmetic error on INT overflow, non-finite FLOAT results and zero divisors.
Value addValues(const Value& a, const Value& b);
Value subtractValues(const Value& a, const Value& b);
Value multiplyValues(const Value& a, const Value& b);
Value divideValues(const Value& a, const Value& b);
Value moduloValues(const Value& a, const Value& b);
Value negateValue(const Value& v);
Value concatValues(const Value& a, const Value& b);

// SQL LIKE with % and _; matching is case-sensitive and works on bytes.
bool likeMatch(std::string_view text, std::string_view pattern);

// Explicit conversion, as in CAST(x AS type). NULL stays NULL.
Value castValue(const Value& v, Type target);
// Implicit conversion when storing into a column of the given type: INT widens
// to FLOAT and whole FLOATs narrow to INT; everything else must already match.
// Returns nullopt when the value does not fit the column.
std::optional<Value> assignValue(const Value& v, Type target);

// Strict text-to-number parsing; surrounding spaces are allowed, junk is not.
std::optional<int64_t> parseInt(std::string_view text);
std::optional<double> parseFloat(std::string_view text);

// ASCII-only case helpers. SQL identifiers and keywords are matched with these.
std::string toLowerAscii(std::string_view text);
std::string toUpperAscii(std::string_view text);
bool equalsIgnoreCase(std::string_view a, std::string_view b);

}  // namespace minisql
