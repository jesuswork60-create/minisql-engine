#include "minisql/lexer.h"

#include "test_util.h"

using namespace minisql;

static std::vector<Token> lex(const std::string& sql) {
  std::vector<Token> tokens = tokenize(sql);
  if (!tokens.empty() && tokens.back().kind == TokenKind::End) tokens.pop_back();
  return tokens;
}

TEST(keywords_are_upper_cased_and_identifiers_keep_their_case) {
  auto tokens = lex("select Name FROM users");
  CHECK_EQ(tokens.size(), size_t{4});
  CHECK(tokens[0].kind == TokenKind::Keyword);
  CHECK_EQ(tokens[0].text, std::string("SELECT"));
  CHECK(tokens[1].kind == TokenKind::Identifier);
  CHECK_EQ(tokens[1].text, std::string("Name"));
  CHECK(tokens[2].kind == TokenKind::Keyword);
  CHECK(tokens[3].kind == TokenKind::Identifier);
}

TEST(numbers) {
  auto tokens = lex("42 3.5 .5 1e3 2.5E-2");
  CHECK_EQ(tokens.size(), size_t{5});
  CHECK(tokens[0].kind == TokenKind::IntLiteral);
  for (size_t i = 1; i < tokens.size(); ++i) CHECK(tokens[i].kind == TokenKind::FloatLiteral);
  CHECK_EQ(tokens[3].text, std::string("1e3"));
}

TEST(digits_running_into_a_name_are_rejected) { CHECK_ERROR(tokenize("SELECT 12abc"), Syntax); }

TEST(string_with_doubled_quote) {
  auto tokens = lex("'it''s'");
  CHECK_EQ(tokens.size(), size_t{1});
  CHECK(tokens[0].kind == TokenKind::StringLiteral);
  CHECK_EQ(tokens[0].text, std::string("it's"));
}

TEST(string_may_span_lines) {
  auto tokens = lex("'a\nb' x");
  CHECK_EQ(tokens[0].text, std::string("a\nb"));
  CHECK_EQ(tokens[1].pos.line, 2);
  CHECK_EQ(tokens[1].pos.column, 4);
}

TEST(unterminated_string_points_at_its_start) {
  try {
    tokenize("SELECT\n  'abc");
    CHECK(false);
  } catch (const Error& e) {
    CHECK(e.kind() == ErrorKind::Syntax);
    CHECK_EQ(e.pos().line, 2);
    CHECK_EQ(e.pos().column, 3);
  }
}

TEST(quoted_identifier) {
  auto tokens = lex("\"order \"\"x\"\"\"");
  CHECK(tokens[0].kind == TokenKind::QuotedIdentifier);
  CHECK_EQ(tokens[0].text, std::string("order \"x\""));
  CHECK_ERROR(tokenize("\"open"), Syntax);
}

TEST(multi_character_symbols) {
  auto tokens = lex("<= >= <> != || < > =");
  std::vector<std::string> expected{"<=", ">=", "<>", "!=", "||", "<", ">", "="};
  CHECK_EQ(tokens.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    CHECK(tokens[i].kind == TokenKind::Symbol);
    CHECK_EQ(tokens[i].text, expected[i]);
  }
}

TEST(comments_are_skipped_and_positions_stay_right) {
  auto tokens = lex("-- first\n/* two\nlines */ x -- end");
  CHECK_EQ(tokens.size(), size_t{1});
  CHECK_EQ(tokens[0].text, std::string("x"));
  CHECK_EQ(tokens[0].pos.line, 3);
  CHECK_EQ(tokens[0].pos.column, 10);
}

TEST(unterminated_block_comment) { CHECK_ERROR(tokenize("/* never closed"), Syntax); }

TEST(unexpected_character_has_position) {
  try {
    tokenize("SELECT\n  @");
    CHECK(false);
  } catch (const Error& e) {
    CHECK_EQ(e.pos().line, 2);
    CHECK_EQ(e.pos().column, 3);
  }
}

TEST(token_stream_ends_with_end_token) {
  auto tokens = tokenize("");
  CHECK_EQ(tokens.size(), size_t{1});
  CHECK(tokens[0].kind == TokenKind::End);
}

TEST(reserved_words) {
  CHECK(isReservedWord("select"));
  CHECK(isReservedWord("SELECT"));
  CHECK(!isReservedWord("users"));
}

TEST(scan_finds_statement_ends) {
  CHECK(scanSql("SELECT 1;").endsStatement);
  CHECK(scanSql("SELECT 1;  -- trailing note\n").endsStatement);
  CHECK(!scanSql("SELECT 1").endsStatement);
  CHECK(!scanSql("SELECT ';'").endsStatement);
  CHECK(!scanSql("SELECT 1; SELECT").endsStatement);
}

TEST(scan_reports_unfinished_text) {
  CHECK(scanSql("SELECT 'abc").unterminated);
  CHECK(scanSql("/* abc").unterminated);
  CHECK(scanSql("SELECT \"abc").unterminated);
  CHECK(!scanSql("SELECT 'it''s';").unterminated);
}

TEST(scan_sees_comments_as_no_content) {
  CHECK(!scanSql("  -- only a note\n").hasContent);
  CHECK(!scanSql("/* note */").hasContent);
  CHECK(scanSql("/* note */ SELECT").hasContent);
}

TEST_MAIN
