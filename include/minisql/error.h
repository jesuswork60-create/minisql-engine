#pragma once

#include <exception>
#include <string>
#include <string_view>

namespace minisql {

struct SourcePos {
  int line = 0;    // 1-based; 0 means the position is unknown
  int column = 0;  // 1-based byte offset within the line; 0 means unknown
  bool known() const { return line > 0; }
};

enum class ErrorKind {
  Syntax,       // lexer and parser
  Catalog,      // unknown or duplicate tables, columns, indexes
  Type,         // operands or values of the wrong type
  Constraint,   // NOT NULL, PRIMARY KEY, UNIQUE
  Arithmetic,   // overflow, division by zero
  Transaction,  // misuse of BEGIN / COMMIT / ROLLBACK
  Io,           // files and CSV input
};

// Every failure the engine reports is an Error. Errors raised deep inside
// value operations carry no position; the expression or statement that was
// running attaches one on the way out (see locate()).
class Error : public std::exception {
 public:
  Error(ErrorKind kind, std::string message, SourcePos pos = {});

  const char* what() const noexcept override { return text_.c_str(); }
  ErrorKind kind() const noexcept { return kind_; }
  const std::string& message() const noexcept { return message_; }
  SourcePos pos() const noexcept { return pos_; }

  // Sets the position unless an inner frame already set a more precise one.
  void locate(SourcePos pos);

 private:
  void rebuildText();

  ErrorKind kind_;
  std::string message_;
  SourcePos pos_;
  std::string text_;
};

// Runs f, attaching pos to any Error that escapes without a position.
template <class F>
auto locating(SourcePos pos, F&& f) -> decltype(f()) {
  try {
    return f();
  } catch (Error& e) {
    e.locate(pos);
    throw;
  }
}

// Formats the error followed by the offending source line and a caret.
// lineOffset shifts the reported line, for statements cut out of a longer file.
std::string renderError(const Error& error, std::string_view source, int lineOffset = 0);

}  // namespace minisql
