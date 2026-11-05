#include "minisql/error.h"

#include <utility>

namespace minisql {

namespace {

const char* kindLabel(ErrorKind kind) {
  switch (kind) {
    case ErrorKind::Syntax: return "syntax error";
    case ErrorKind::Catalog: return "catalog error";
    case ErrorKind::Type: return "type error";
    case ErrorKind::Constraint: return "constraint error";
    case ErrorKind::Arithmetic: return "arithmetic error";
    case ErrorKind::Transaction: return "transaction error";
    case ErrorKind::Io: return "i/o error";
  }
  return "error";
}

std::string describe(ErrorKind kind, const std::string& message, SourcePos pos) {
  std::string text = kindLabel(kind);
  if (pos.known()) {
    text += " at line " + std::to_string(pos.line);
    if (pos.column > 0) text += ", column " + std::to_string(pos.column);
  }
  return text + ": " + message;
}

std::string_view lineAt(std::string_view source, int line) {
  size_t start = 0;
  for (int current = 1; current < line; ++current) {
    size_t newline = source.find('\n', start);
    if (newline == std::string_view::npos) return {};
    start = newline + 1;
  }
  size_t end = source.find('\n', start);
  std::string_view text = source.substr(start, end == std::string_view::npos ? end : end - start);
  if (!text.empty() && text.back() == '\r') text.remove_suffix(1);
  return text;
}

}  // namespace

Error::Error(ErrorKind kind, std::string message, SourcePos pos)
    : kind_(kind), message_(std::move(message)), pos_(pos) {
  rebuildText();
}

void Error::locate(SourcePos pos) {
  if (pos_.known()) return;
  pos_ = pos;
  rebuildText();
}

void Error::rebuildText() { text_ = describe(kind_, message_, pos_); }

std::string renderError(const Error& error, std::string_view source, int lineOffset) {
  SourcePos pos = error.pos();
  if (!pos.known()) return error.what();

  SourcePos shifted{pos.line + lineOffset, pos.column};
  std::string text = describe(error.kind(), error.message(), shifted);
  std::string_view line = lineAt(source, pos.line);
  if (line.empty() || pos.column <= 0) return text;

  text += "\n  ";
  text += line;
  text += "\n  ";
  // Keep tabs so the caret lines up however the terminal expands them.
  for (int i = 0; i + 1 < pos.column; ++i) {
    bool isTab = static_cast<size_t>(i) < line.size() && line[static_cast<size_t>(i)] == '\t';
    text += isTab ? '\t' : ' ';
  }
  text += '^';
  return text;
}

}  // namespace minisql
