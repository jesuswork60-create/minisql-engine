#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "minisql/error.h"

namespace minisql {

enum class TokenKind {
  Identifier,        // text as written
  QuotedIdentifier,  // "name", text without quotes
  Keyword,           // text upper-cased
  IntLiteral,        // text is the digits
  FloatLiteral,      // text is the number as written
  StringLiteral,     // text with '' unescaped
  Symbol,            // operators and punctuation
  End,
};

struct Token {
  TokenKind kind = TokenKind::End;
  std::string text;
  SourcePos pos;
};

// Splits SQL into tokens, ending with one End token. Skips whitespace, -- line
// comments and /* block comments */. Throws a syntax error on invalid input.
std::vector<Token> tokenize(std::string_view sql);

bool isReservedWord(std::string_view word);

// What a shell needs to decide whether to keep reading lines. This only tracks
// quoting and comments, so it also works on input that would not tokenize.
struct SqlScan {
  bool hasContent = false;     // something other than whitespace and comments
  bool endsStatement = false;  // the last significant character is a closing ;
  bool unterminated = false;   // input stopped inside a string, name or comment
};
SqlScan scanSql(std::string_view sql);

}  // namespace minisql
