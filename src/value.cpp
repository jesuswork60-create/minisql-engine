#include "minisql/value.h"

#include <charconv>
#include <cmath>
#include <functional>
#include <limits>

#include "minisql/error.h"

namespace minisql {

namespace {

constexpr double kTwo63 = 9223372036854775808.0;

bool isWholeInRange(double d) { return d >= -kTwo63 && d < kTwo63 && std::trunc(d) == d; }

std::string formatFloat(double d) {
  char buffer[64];
  auto [end, ec] = std::to_chars(buffer, buffer + sizeof buffer, d);
  (void)ec;  // the buffer always fits the shortest round-trip form of a double
  std::string text(buffer, end);
  if (text.find_first_of(".e") == std::string::npos) text += ".0";
  return text;
}

// Compares without widening the INT, which would lose precision above 2^53.
int compareIntFloat(int64_t i, double d) {
  if (d >= kTwo63) return -1;
  if (d < -kTwo63) return 1;
  auto whole = static_cast<int64_t>(d);
  if (i != whole) return i < whole ? -1 : 1;
  double fraction = d - static_cast<double>(whole);
  if (fraction > 0) return -1;
  if (fraction < 0) return 1;
  return 0;
}

[[noreturn]] void throwNotComparable(const std::string& a, const std::string& b) {
  throw Error(ErrorKind::Type, "cannot compare " + a + " and " + b);
}

[[noreturn]] void throwBadOperands(const char* op, const Value& a, const Value& b) {
  throw Error(ErrorKind::Type,
              std::string("operator ") + op + " cannot take " + a.typeLabel() + " and " + b.typeLabel());
}

double checkedFloat(double d) {
  if (!std::isfinite(d)) throw Error(ErrorKind::Arithmetic, "FLOAT result out of range");
  return d;
}

[[noreturn]] void throwIntOverflow() { throw Error(ErrorKind::Arithmetic, "INT result out of range"); }

template <class IntOp, class FloatOp>
Value arithmetic(const char* symbol, const Value& a, const Value& b, IntOp intOp, FloatOp floatOp) {
  if (a.isNull() || b.isNull()) {
    const Value& other = a.isNull() ? b : a;
    if (!other.isNull() && !other.isNumeric()) throwBadOperands(symbol, a, b);
    return Value::Null();
  }
  if (!a.isNumeric() || !b.isNumeric()) throwBadOperands(symbol, a, b);
  if (a.isInt() && b.isInt()) return Value::Int(intOp(a.asInt(), b.asInt()));
  return Value::Float(checkedFloat(floatOp(a.toDouble(), b.toDouble())));
}

std::string_view trimSpaces(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

std::optional<bool> parseBool(std::string_view raw) {
  std::string text = toLowerAscii(trimSpaces(raw));
  if (text == "true" || text == "t" || text == "1" || text == "yes") return true;
  if (text == "false" || text == "f" || text == "0" || text == "no") return false;
  return std::nullopt;
}

char asciiLower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
char asciiUpper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }

}  // namespace

std::string_view typeName(Type type) {
  switch (type) {
    case Type::Int: return "INT";
    case Type::Float: return "FLOAT";
    case Type::Text: return "TEXT";
    case Type::Bool: return "BOOL";
  }
  return "?";
}

std::optional<Type> typeFromName(std::string_view name) {
  std::string upper = toUpperAscii(name);
  if (upper == "INT" || upper == "INTEGER" || upper == "BIGINT") return Type::Int;
  if (upper == "FLOAT" || upper == "REAL" || upper == "DOUBLE") return Type::Float;
  if (upper == "TEXT" || upper == "VARCHAR" || upper == "CHAR" || upper == "STRING") return Type::Text;
  if (upper == "BOOL" || upper == "BOOLEAN") return Type::Bool;
  return std::nullopt;
}

std::optional<Type> Value::type() const {
  if (isInt()) return Type::Int;
  if (isFloat()) return Type::Float;
  if (isText()) return Type::Text;
  if (isBool()) return Type::Bool;
  return std::nullopt;
}

std::string Value::typeLabel() const {
  auto t = type();
  return t ? std::string(typeName(*t)) : "NULL";
}

std::string Value::toString() const {
  if (isNull()) return "NULL";
  if (isInt()) return std::to_string(asInt());
  if (isFloat()) return formatFloat(asFloat());
  if (isBool()) return asBool() ? "true" : "false";
  return asText();
}

std::string Value::toSql() const {
  if (isBool()) return asBool() ? "TRUE" : "FALSE";
  if (!isText()) return toString();
  std::string out = "'";
  for (char c : asText()) {
    if (c == '\'') out += '\'';
    out += c;
  }
  return out + "'";
}

bool sameKey(const Value& a, const Value& b) {
  if (a.isNull() || b.isNull()) return a.isNull() && b.isNull();
  if (a.isNumeric() && b.isNumeric()) return compareValues(a, b) == 0;
  if (a.isText() && b.isText()) return a.asText() == b.asText();
  if (a.isBool() && b.isBool()) return a.asBool() == b.asBool();
  return false;
}

size_t hashValue(const Value& v) {
  if (v.isNull()) return 0x9e3779b9u;
  if (v.isInt()) return std::hash<int64_t>{}(v.asInt());
  if (v.isFloat()) {
    // Whole floats must hash like the equal INT because sameKey treats them as one key.
    double d = v.asFloat();
    if (isWholeInRange(d)) return std::hash<int64_t>{}(static_cast<int64_t>(d));
    return std::hash<double>{}(d);
  }
  if (v.isBool()) return v.asBool() ? 0x51ed270bu : 0x2f0b4a87u;
  return std::hash<std::string>{}(v.asText());
}

size_t RowHash::operator()(const Row& row) const {
  size_t seed = row.size();
  for (const Value& v : row) seed ^= hashValue(v) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
  return seed;
}

bool RowKeyEq::operator()(const Row& a, const Row& b) const {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (!sameKey(a[i], b[i])) return false;
  }
  return true;
}

int compareValues(const Value& a, const Value& b) {
  if (a.isNumeric() && b.isNumeric()) {
    if (a.isInt() && b.isInt()) {
      return a.asInt() < b.asInt() ? -1 : a.asInt() > b.asInt() ? 1 : 0;
    }
    if (a.isInt()) return compareIntFloat(a.asInt(), b.asFloat());
    if (b.isInt()) return -compareIntFloat(b.asInt(), a.asFloat());
    return a.asFloat() < b.asFloat() ? -1 : a.asFloat() > b.asFloat() ? 1 : 0;
  }
  if (a.isText() && b.isText()) {
    int order = a.asText().compare(b.asText());
    return order < 0 ? -1 : order > 0 ? 1 : 0;
  }
  if (a.isBool() && b.isBool()) return static_cast<int>(a.asBool()) - static_cast<int>(b.asBool());
  throwNotComparable(a.typeLabel(), b.typeLabel());
}

void requireComparable(const Value& key, Type columnType) {
  bool numericColumn = columnType == Type::Int || columnType == Type::Float;
  bool ok = (numericColumn && key.isNumeric()) || (columnType == Type::Text && key.isText()) ||
            (columnType == Type::Bool && key.isBool());
  if (!ok) throwNotComparable(std::string(typeName(columnType)), key.typeLabel());
}

std::optional<bool> truthOf(const Value& v) {
  if (v.isNull()) return std::nullopt;
  if (v.isBool()) return v.asBool();
  throw Error(ErrorKind::Type, "expected a BOOL condition, got " + v.typeLabel());
}

Value fromTruth(std::optional<bool> truth) { return truth ? Value::Bool(*truth) : Value::Null(); }

Value logicalNot(const Value& v) {
  auto truth = truthOf(v);
  return truth ? Value::Bool(!*truth) : Value::Null();
}

Value compareSql(CompareOp op, const Value& a, const Value& b) {
  if (a.isNull() || b.isNull()) return Value::Null();
  int order = compareValues(a, b);
  switch (op) {
    case CompareOp::Eq: return Value::Bool(order == 0);
    case CompareOp::Ne: return Value::Bool(order != 0);
    case CompareOp::Lt: return Value::Bool(order < 0);
    case CompareOp::Le: return Value::Bool(order <= 0);
    case CompareOp::Gt: return Value::Bool(order > 0);
    case CompareOp::Ge: return Value::Bool(order >= 0);
  }
  return Value::Null();
}

Value addValues(const Value& a, const Value& b) {
  return arithmetic(
      "+", a, b,
      [](int64_t x, int64_t y) {
        int64_t r;
        if (__builtin_add_overflow(x, y, &r)) throwIntOverflow();
        return r;
      },
      [](double x, double y) { return x + y; });
}

Value subtractValues(const Value& a, const Value& b) {
  return arithmetic(
      "-", a, b,
      [](int64_t x, int64_t y) {
        int64_t r;
        if (__builtin_sub_overflow(x, y, &r)) throwIntOverflow();
        return r;
      },
      [](double x, double y) { return x - y; });
}

Value multiplyValues(const Value& a, const Value& b) {
  return arithmetic(
      "*", a, b,
      [](int64_t x, int64_t y) {
        int64_t r;
        if (__builtin_mul_overflow(x, y, &r)) throwIntOverflow();
        return r;
      },
      [](double x, double y) { return x * y; });
}

Value divideValues(const Value& a, const Value& b) {
  auto zero = []() { throw Error(ErrorKind::Arithmetic, "division by zero"); };
  return arithmetic(
      "/", a, b,
      [&](int64_t x, int64_t y) {
        if (y == 0) zero();
        if (x == std::numeric_limits<int64_t>::min() && y == -1) throwIntOverflow();
        return x / y;
      },
      [&](double x, double y) {
        if (y == 0.0) zero();
        return x / y;
      });
}

Value moduloValues(const Value& a, const Value& b) {
  auto zero = []() { throw Error(ErrorKind::Arithmetic, "division by zero"); };
  return arithmetic(
      "%", a, b,
      [&](int64_t x, int64_t y) -> int64_t {
        if (y == 0) zero();
        if (y == -1) return 0;  // INT64_MIN % -1 traps on x86 otherwise
        return x % y;
      },
      [&](double x, double y) {
        if (y == 0.0) zero();
        return std::fmod(x, y);
      });
}

Value negateValue(const Value& v) {
  if (v.isNull()) return Value::Null();
  if (v.isInt()) {
    if (v.asInt() == std::numeric_limits<int64_t>::min()) throwIntOverflow();
    return Value::Int(-v.asInt());
  }
  if (v.isFloat()) return Value::Float(-v.asFloat());
  throw Error(ErrorKind::Type, "unary minus cannot take " + v.typeLabel());
}

Value concatValues(const Value& a, const Value& b) {
  if (a.isNull() || b.isNull()) return Value::Null();
  return Value::Text(a.toString() + b.toString());
}

bool likeMatch(std::string_view text, std::string_view pattern) {
  // Greedy two-pointer match. Only the most recent % can ever need to backtrack,
  // so this stays O(text * pattern) however many wildcards the pattern has.
  size_t t = 0, p = 0;
  size_t starPattern = std::string_view::npos, starText = 0;
  while (t < text.size()) {
    if (p < pattern.size() && pattern[p] == '%') {
      starPattern = p++;
      starText = t;
    } else if (p < pattern.size() && (pattern[p] == '_' || pattern[p] == text[t])) {
      ++p;
      ++t;
    } else if (starPattern != std::string_view::npos) {
      p = starPattern + 1;
      t = ++starText;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '%') ++p;
  return p == pattern.size();
}

Value castValue(const Value& v, Type target) {
  if (v.isNull()) return Value::Null();
  auto fail = [&]() -> Error {
    return Error(ErrorKind::Type,
                 "cannot convert " + v.toSql() + " to " + std::string(typeName(target)));
  };
  switch (target) {
    case Type::Int:
      if (v.isInt()) return v;
      if (v.isBool()) return Value::Int(v.asBool() ? 1 : 0);
      if (v.isFloat()) {
        double d = std::trunc(v.asFloat());
        if (!(d >= -kTwo63 && d < kTwo63)) throw fail();  // also rejects NaN
        return Value::Int(static_cast<int64_t>(d));
      }
      if (auto parsed = parseInt(v.asText())) return Value::Int(*parsed);
      throw fail();
    case Type::Float:
      if (v.isFloat()) return v;
      if (v.isInt()) return Value::Float(static_cast<double>(v.asInt()));
      if (v.isBool()) return Value::Float(v.asBool() ? 1.0 : 0.0);
      if (auto parsed = parseFloat(v.asText())) return Value::Float(*parsed);
      throw fail();
    case Type::Text:
      return v.isText() ? v : Value::Text(v.toString());
    case Type::Bool:
      if (v.isBool()) return v;
      if (v.isInt()) return Value::Bool(v.asInt() != 0);
      if (v.isFloat()) return Value::Bool(v.asFloat() != 0.0);
      if (auto parsed = parseBool(v.asText())) return Value::Bool(*parsed);
      throw fail();
  }
  throw fail();
}

std::optional<Value> assignValue(const Value& v, Type target) {
  if (v.isNull() || v.type() == target) return v;
  if (target == Type::Float && v.isInt()) return Value::Float(static_cast<double>(v.asInt()));
  if (target == Type::Int && v.isFloat() && isWholeInRange(v.asFloat())) {
    return Value::Int(static_cast<int64_t>(v.asFloat()));
  }
  return std::nullopt;
}

std::optional<int64_t> parseInt(std::string_view raw) {
  std::string_view text = trimSpaces(raw);
  if (!text.empty() && text.front() == '+') text.remove_prefix(1);
  if (text.empty()) return std::nullopt;
  int64_t out = 0;
  auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
  if (ec != std::errc() || end != text.data() + text.size()) return std::nullopt;
  return out;
}

std::optional<double> parseFloat(std::string_view raw) {
  std::string_view text = trimSpaces(raw);
  if (!text.empty() && text.front() == '+') text.remove_prefix(1);
  if (text.empty()) return std::nullopt;
  double out = 0;
  auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
  if (ec != std::errc() || end != text.data() + text.size() || !std::isfinite(out)) return std::nullopt;
  return out;
}

std::string toLowerAscii(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = asciiLower(c);
  return out;
}

std::string toUpperAscii(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = asciiUpper(c);
  return out;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (asciiLower(a[i]) != asciiLower(b[i])) return false;
  }
  return true;
}

}  // namespace minisql
