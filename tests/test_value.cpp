#include "minisql/value.h"

#include <cmath>
#include <limits>

#include "test_util.h"

using namespace minisql;

TEST(null_compares_to_unknown) {
  CHECK(compareSql(CompareOp::Eq, Value::Null(), Value::Int(1)).isNull());
  CHECK(compareSql(CompareOp::Ne, Value::Int(1), Value::Null()).isNull());
  CHECK(compareSql(CompareOp::Eq, Value::Null(), Value::Null()).isNull());
}

TEST(int_and_float_compare_by_value) {
  CHECK(compareSql(CompareOp::Eq, Value::Int(2), Value::Float(2.0)).asBool());
  CHECK(compareSql(CompareOp::Lt, Value::Int(2), Value::Float(2.5)).asBool());
}

TEST(huge_int_float_comparison_is_exact) {
  // 2^53 + 1 has no exact double; a lossy comparison would call these equal.
  int64_t big = (int64_t{1} << 53) + 1;
  CHECK(!compareSql(CompareOp::Eq, Value::Int(big), Value::Float(9007199254740992.0)).asBool());
  CHECK(compareSql(CompareOp::Gt, Value::Int(big), Value::Float(9007199254740992.0)).asBool());
  // INT64_MAX converted to double rounds up to 2^63, which is above every int.
  CHECK(compareSql(CompareOp::Lt, Value::Int(std::numeric_limits<int64_t>::max()), Value::Float(9223372036854775808.0))
            .asBool());
}

TEST(comparing_different_kinds_is_a_type_error) {
  CHECK_ERROR(compareSql(CompareOp::Eq, Value::Int(1), Value::Text("1")), Type);
  CHECK_ERROR(compareSql(CompareOp::Lt, Value::Bool(true), Value::Int(1)), Type);
}

TEST(text_and_bool_ordering) {
  CHECK(compareValues(Value::Text("a"), Value::Text("b")) < 0);
  CHECK(compareValues(Value::Bool(false), Value::Bool(true)) < 0);
}

TEST(three_valued_not) {
  CHECK_EQ(logicalNot(Value::Bool(true)).toString(), std::string("false"));
  CHECK(logicalNot(Value::Null()).isNull());
  CHECK_ERROR(logicalNot(Value::Int(1)), Type);
}

TEST(truth_of_rejects_non_bool) {
  CHECK(!truthOf(Value::Null()).has_value());
  CHECK(*truthOf(Value::Bool(true)));
  CHECK_ERROR(truthOf(Value::Text("true")), Type);
}

TEST(arithmetic_keeps_int_when_it_can) {
  CHECK(addValues(Value::Int(1), Value::Int(2)).isInt());
  CHECK(addValues(Value::Int(1), Value::Float(2)).isFloat());
  CHECK_EQ(divideValues(Value::Int(7), Value::Int(2)).asInt(), int64_t{3});
  CHECK_EQ(divideValues(Value::Int(-7), Value::Int(2)).asInt(), int64_t{-3});
  CHECK_EQ(moduloValues(Value::Int(-7), Value::Int(3)).asInt(), int64_t{-1});
  CHECK_EQ(divideValues(Value::Float(7), Value::Int(2)).asFloat(), 3.5);
}

TEST(arithmetic_propagates_null) {
  CHECK(addValues(Value::Null(), Value::Int(1)).isNull());
  CHECK(multiplyValues(Value::Int(1), Value::Null()).isNull());
  CHECK(negateValue(Value::Null()).isNull());
  CHECK(divideValues(Value::Null(), Value::Int(0)).isNull());
}

TEST(overflow_is_an_error_rather_than_a_wrap) {
  const int64_t max = std::numeric_limits<int64_t>::max();
  const int64_t min = std::numeric_limits<int64_t>::min();
  CHECK_ERROR(addValues(Value::Int(max), Value::Int(1)), Arithmetic);
  CHECK_ERROR(subtractValues(Value::Int(min), Value::Int(1)), Arithmetic);
  CHECK_ERROR(multiplyValues(Value::Int(max), Value::Int(2)), Arithmetic);
  CHECK_ERROR(negateValue(Value::Int(min)), Arithmetic);
  CHECK_ERROR(divideValues(Value::Int(min), Value::Int(-1)), Arithmetic);
  CHECK_EQ(moduloValues(Value::Int(min), Value::Int(-1)).asInt(), int64_t{0});
}

TEST(division_by_zero) {
  CHECK_ERROR(divideValues(Value::Int(1), Value::Int(0)), Arithmetic);
  CHECK_ERROR(moduloValues(Value::Int(1), Value::Int(0)), Arithmetic);
  CHECK_ERROR(divideValues(Value::Float(1), Value::Float(0)), Arithmetic);
}

TEST(arithmetic_on_text_is_a_type_error) {
  CHECK_ERROR(addValues(Value::Text("1"), Value::Int(1)), Type);
  CHECK_ERROR(negateValue(Value::Text("x")), Type);
}

TEST(concat) {
  CHECK_EQ(concatValues(Value::Text("a"), Value::Int(1)).asText(), std::string("a1"));
  CHECK(concatValues(Value::Text("a"), Value::Null()).isNull());
}

TEST(like_patterns) {
  CHECK(likeMatch("hello", "h%"));
  CHECK(likeMatch("hello", "%llo"));
  CHECK(likeMatch("hello", "h_llo"));
  CHECK(likeMatch("hello", "%"));
  CHECK(likeMatch("", "%"));
  CHECK(!likeMatch("", "_"));
  CHECK(!likeMatch("hello", "h"));
  CHECK(likeMatch("a%b", "a%b"));
  CHECK(likeMatch("abcabc", "%b%c"));
  CHECK(!likeMatch("abc", "%d%"));
}

TEST(like_stays_fast_on_many_wildcards) {
  // An exponential matcher would not finish this.
  std::string text(2000, 'a');
  std::string pattern;
  for (int i = 0; i < 40; ++i) pattern += "%a";
  CHECK(!likeMatch(text, pattern + "b"));
  CHECK(likeMatch(text, pattern));
}

TEST(cast_conversions) {
  CHECK_EQ(castValue(Value::Text("12"), Type::Int).asInt(), int64_t{12});
  CHECK_EQ(castValue(Value::Text(" 12 "), Type::Int).asInt(), int64_t{12});
  CHECK_EQ(castValue(Value::Int(3), Type::Float).asFloat(), 3.0);
  CHECK_EQ(castValue(Value::Int(3), Type::Text).asText(), std::string("3"));
  CHECK(castValue(Value::Null(), Type::Int).isNull());
  CHECK(castValue(Value::Text("TRUE"), Type::Bool).asBool());
  CHECK_ERROR(castValue(Value::Text("abc"), Type::Int), Type);
  CHECK_ERROR(castValue(Value::Text("abc"), Type::Bool), Type);
}

TEST(cast_float_to_int_stays_in_range) {
  CHECK_ERROR(castValue(Value::Float(1e30), Type::Int), Type);
  CHECK_ERROR(castValue(Value::Float(std::nan("")), Type::Int), Type);
}

TEST(assignment_rules) {
  CHECK(assignValue(Value::Int(3), Type::Float)->isFloat());
  CHECK(assignValue(Value::Null(), Type::Int)->isNull());
  CHECK(!assignValue(Value::Text("3"), Type::Int).has_value());
  CHECK(!assignValue(Value::Int(1), Type::Bool).has_value());
  CHECK(!assignValue(Value::Int(1), Type::Text).has_value());
}

TEST(key_equality_across_numeric_types) {
  CHECK(sameKey(Value::Int(2), Value::Float(2.0)));
  CHECK_EQ(hashValue(Value::Int(2)), hashValue(Value::Float(2.0)));
  CHECK(!sameKey(Value::Int(2), Value::Text("2")));
  CHECK(sameKey(Value::Null(), Value::Null()));
  CHECK(!sameKey(Value::Int((int64_t{1} << 53) + 1), Value::Float(9007199254740992.0)));
}

TEST(number_parsing) {
  CHECK_EQ(*parseInt("-5"), int64_t{-5});
  CHECK(!parseInt("5x").has_value());
  CHECK(!parseInt("").has_value());
  CHECK(!parseInt("99999999999999999999").has_value());
  CHECK_EQ(*parseFloat("2.5"), 2.5);
  CHECK(!parseFloat("abc").has_value());
}

TEST(sql_text_round_trips) {
  CHECK_EQ(Value::Text("it's").toSql(), std::string("'it''s'"));
  CHECK_EQ(Value::Null().toSql(), std::string("NULL"));
  CHECK_EQ(Value::Bool(true).toSql(), std::string("TRUE"));
  CHECK_EQ(Value::Float(2.0).toString(), std::string("2.0"));
}

TEST_MAIN
