#include "minisql/lexer.h"

#include <algorithm>
#include <iterator>

#include "minisql/value.h"

namespace minisql {

namespace {

// Sorted, for binary search. Some of these (LEFT, DISTINCT, UNION, ...) name
// features the engine does not have; reserving them gives a clear syntax error
// instead of silently reading "FROM a LEFT JOIN b" as a table alias.
constexpr std::string_view kKeywords[] = {
    "ALL",     "AND",     "AS",      "ASC",     "BEGIN",  "BETWEEN",     "BY",     "CASE",   "CAST",
    "COMMIT",  "CREATE",  "CROSS",   "DEFAULT", "DELETE", "DESC",        "DISTINCT", "DROP", "EXISTS",
    "EXPLAIN", "FALSE",   "FROM",    "FULL",    "GROUP",  "HAVING",      "IF",     "IN",     "INDEX",
    "INNER",   "INSERT",  "INTO",    "IS",      "JOIN",   "KEY",         "LEFT",   "LIKE",   "LIMIT",
    "NOT",     "NULL",    "OFFSET",  "ON",      "OR",     "ORDER",       "OUTER",  "PRIMARY", "RIGHT",
    "ROLLBACK", "SELECT", "SET",     "TABLE",   "TRANSACTION", "TRUE",   "UNION",  "UNIQUE", "UPDATE",
    "USING",   "VALUES",  "WHEN",    "WHERE",
};

bool isIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c & 0x80); }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isIdentPart(char c) { return isIdentStart(c) || isDigit(c); }

class Lexer {
 public:
  explicit Lexer(std::string_view sql) : sql_(sql) {}

  std::vector<Token> run() {
    std::vector<Token> tokens;
    for (;;) {
      skipTrivia();
      if (atEnd()) break;
      tokens.push_back(nextToken());
    }
    tokens.push_back(Token{TokenKind::End, "", here()});
    return tokens;
  }

 private:
  bool atEnd() const { return index_ >= sql_.size(); }
  char peek(size_t ahead = 0) const { return index_ + ahead < sql_.size() ? sql_[index_ + ahead] : '\0'; }
  SourcePos here() const { return SourcePos{line_, column_}; }

  char advance() {
    char c = sql_[index_++];
    if (c == '\n') {
      ++line_;
      column_ = 1;
    } else {
      ++column_;
    }
    return c;
  }

  void skipTrivia() {
    for (;;) {
      char c = peek();
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        advance();
      } else if (c == '-' && peek(1) == '-') {
        while (!atEnd() && peek() != '\n') advance();
      } else if (c == '/' && peek(1) == '*') {
        SourcePos start = here();
        advance();
        advance();
        while (!(peek() == '*' && peek(1) == '/')) {
          if (atEnd()) throw Error(ErrorKind::Syntax, "unterminated block comment", start);
          advance();
        }
        advance();
        advance();
      } else {
        return;
      }
    }
  }

  Token nextToken() {
    char c = peek();
    if (isIdentStart(c)) return readWord();
    if (isDigit(c) || (c == '.' && isDigit(peek(1)))) return readNumber();
    if (c == '\'') return readQuoted('\'', TokenKind::StringLiteral, "string literal");
    if (c == '"') return readQuoted('"', TokenKind::QuotedIdentifier, "quoted identifier");
    return readSymbol();
  }

  Token readWord() {
    SourcePos pos = here();
    size_t start = index_;
    while (!atEnd() && isIdentPart(peek())) advance();
    std::string word(sql_.substr(start, index_ - start));
    if (isReservedWord(word)) return Token{TokenKind::Keyword, toUpperAscii(word), pos};
    return Token{TokenKind::Identifier, std::move(word), pos};
  }

  Token readNumber() {
    SourcePos pos = here();
    size_t start = index_;
    bool isFloat = false;
    while (isDigit(peek())) advance();
    if (peek() == '.') {
      isFloat = true;
      advance();
      while (isDigit(peek())) advance();
    }
    if (peek() == 'e' || peek() == 'E') {
      size_t signLength = (peek(1) == '+' || peek(1) == '-') ? 1 : 0;
      if (isDigit(peek(1 + signLength))) {
        isFloat = true;
        advance();
        if (signLength) advance();
        while (isDigit(peek())) advance();
      }
    }
    if (isIdentStart(peek())) throw Error(ErrorKind::Syntax, "malformed number", pos);
    std::string text(sql_.substr(start, index_ - start));
    return Token{isFloat ? TokenKind::FloatLiteral : TokenKind::IntLiteral, std::move(text), pos};
  }

  Token readQuoted(char quote, TokenKind kind, const char* what) {
    SourcePos pos = here();
    advance();
    std::string text;
    for (;;) {
      if (atEnd()) throw Error(ErrorKind::Syntax, std::string("unterminated ") + what, pos);
      char c = advance();
      if (c == quote) {
        if (peek() != quote) break;
        advance();  // a doubled quote stands for one quote character
      }
      text += c;
    }
    if (kind == TokenKind::QuotedIdentifier && text.empty()) {
      throw Error(ErrorKind::Syntax, "empty quoted identifier", pos);
    }
    return Token{kind, std::move(text), pos};
  }

  Token readSymbol() {
    SourcePos pos = here();
    char c = peek();
    char next = peek(1);
    bool two = (c == '<' && (next == '=' || next == '>')) || (c == '>' && next == '=') ||
               (c == '!' && next == '=') || (c == '|' && next == '|');
    if (two) {
      std::string text{c, next};
      advance();
      advance();
      return Token{TokenKind::Symbol, std::move(text), pos};
    }
    if (std::string_view("(),;.*+-/%=<>").find(c) == std::string_view::npos) {
      throw Error(ErrorKind::Syntax, std::string("unexpected character '") + c + "'", pos);
    }
    advance();
    return Token{TokenKind::Symbol, std::string(1, c), pos};
  }

  std::string_view sql_;
  size_t index_ = 0;
  int line_ = 1;
  int column_ = 1;
};

}  // namespace

std::vector<Token> tokenize(std::string_view sql) { return Lexer(sql).run(); }

bool isReservedWord(std::string_view word) {
  std::string upper = toUpperAscii(word);
  return std::binary_search(std::begin(kKeywords), std::end(kKeywords), std::string_view(upper));
}

SqlScan scanSql(std::string_view sql) {
  SqlScan scan;
  size_t i = 0;
  while (i < sql.size()) {
    char c = sql[i];
    char next = i + 1 < sql.size() ? sql[i + 1] : '\0';
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      ++i;
    } else if (c == '-' && next == '-') {
      size_t newline = sql.find('\n', i);
      i = newline == std::string_view::npos ? sql.size() : newline + 1;
    } else if (c == '/' && next == '*') {
      size_t close = sql.find("*/", i + 2);
      if (close == std::string_view::npos) {
        scan.unterminated = true;
        return scan;
      }
      i = close + 2;
    } else if (c == '\'' || c == '"') {
      scan.hasContent = true;
      scan.endsStatement = false;
      size_t close = sql.find(c, i + 1);
      // A doubled quote just closes and immediately reopens, so plain find works.
      if (close == std::string_view::npos) {
        scan.unterminated = true;
        return scan;
      }
      i = close + 1;
    } else {
      scan.hasContent = true;
      scan.endsStatement = c == ';';
      ++i;
    }
  }
  return scan;
}

}  // namespace minisql
