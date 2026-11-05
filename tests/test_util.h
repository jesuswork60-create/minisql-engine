#pragma once

// A very small test harness: TEST(name) registers a function, the CHECK macros
// record failures and keep going, and TEST_MAIN runs everything in the file.

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "minisql/database.h"
#include "minisql/error.h"

namespace testutil {

struct TestCase {
  std::string name;
  std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

inline int& failureCount() {
  static int failures = 0;
  return failures;
}

struct Registrar {
  Registrar(std::string name, std::function<void()> body) { registry().push_back({std::move(name), std::move(body)}); }
};

inline void reportFailure(const char* file, int line, const std::string& message) {
  std::cerr << file << ':' << line << ": " << message << '\n';
  ++failureCount();
}

template <typename T>
std::string show(const T& value) {
  std::ostringstream out;
  out << value;
  return out.str();
}

inline std::string show(bool value) { return value ? "true" : "false"; }

inline int runAll() {
  int failedTests = 0;
  for (const TestCase& test : registry()) {
    int before = failureCount();
    try {
      test.body();
    } catch (const std::exception& e) {
      std::cerr << "unexpected exception: " << e.what() << '\n';
      ++failureCount();
    }
    if (failureCount() != before) {
      std::cerr << "FAILED: " << test.name << '\n';
      ++failedTests;
    }
  }
  std::cout << registry().size() - static_cast<size_t>(failedTests) << " of " << registry().size() << " tests passed\n";
  return failedTests == 0 ? 0 : 1;
}

// Rows as "1|a;2|b", which keeps expected results readable on one line.
inline std::string rowsText(const minisql::Result& result) {
  std::string out;
  for (size_t r = 0; r < result.rows.size(); ++r) {
    if (r) out += ';';
    for (size_t c = 0; c < result.rows[r].size(); ++c) {
      if (c) out += '|';
      out += result.rows[r][c].toString();
    }
  }
  return out;
}

inline std::string columnsText(const minisql::Result& result) {
  std::string out;
  for (size_t c = 0; c < result.columns.size(); ++c) {
    if (c) out += '|';
    out += result.columns[c];
  }
  return out;
}

// The plan text of a statement, one line per row.
inline std::string planText(minisql::Database& db, const std::string& sql) {
  std::string out;
  for (const minisql::Row& row : db.query("EXPLAIN " + sql).rows) out += row[0].asText() + "\n";
  return out;
}

inline bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace testutil

#define TEST(name)                                                  \
  static void test_##name();                                        \
  static const testutil::Registrar registrar_##name(#name, test_##name); \
  static void test_##name()

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) testutil::reportFailure(__FILE__, __LINE__, "CHECK failed: " #condition); \
  } while (false)

#define CHECK_EQ(actual, expected)                                                                  \
  do {                                                                                              \
    auto actualValue = (actual);                                                                    \
    decltype(actualValue) expectedValue = (expected);                                               \
    if (!(actualValue == expectedValue)) {                                                          \
      testutil::reportFailure(__FILE__, __LINE__,                                                   \
                              std::string(#actual) + "\n  actual:   " + testutil::show(actualValue) + \
                                  "\n  expected: " + testutil::show(expectedValue));                \
    }                                                                                               \
  } while (false)

// Checks that the statement throws an Error of the given kind.
#define CHECK_ERROR(statement, errorKind)                                                       \
  do {                                                                                          \
    try {                                                                                       \
      statement;                                                                                \
      testutil::reportFailure(__FILE__, __LINE__, "expected an error from: " #statement);       \
    } catch (const minisql::Error& caught) {                                                    \
      if (caught.kind() != minisql::ErrorKind::errorKind) {                                     \
        testutil::reportFailure(__FILE__, __LINE__,                                             \
                                std::string("wrong error kind from: " #statement "\n  got: ") + caught.what()); \
      }                                                                                         \
    }                                                                                           \
  } while (false)

// Checks the error kind and that the message mentions a phrase.
#define CHECK_ERROR_TEXT(statement, errorKind, phrase)                                           \
  do {                                                                                           \
    try {                                                                                        \
      statement;                                                                                 \
      testutil::reportFailure(__FILE__, __LINE__, "expected an error from: " #statement);        \
    } catch (const minisql::Error& caught) {                                                     \
      if (caught.kind() != minisql::ErrorKind::errorKind ||                                      \
          !testutil::contains(caught.what(), phrase)) {                                          \
        testutil::reportFailure(__FILE__, __LINE__,                                              \
                                std::string("wrong error from: " #statement "\n  got: ") + caught.what()); \
      }                                                                                          \
    }                                                                                            \
  } while (false)

#define TEST_MAIN \
  int main() { return testutil::runAll(); }
