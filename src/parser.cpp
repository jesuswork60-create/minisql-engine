#include "minisql/parser.h"

#include <algorithm>
#include <iterator>
#include <utility>

#include "minisql/lexer.h"

namespace minisql {

namespace {

using ast::Expr;
using ast::ExprPtr;

constexpr int kMaxExprHeight = 512;
constexpr int kMaxParseDepth = 200;

// Reserved words that are still fine as table or column names.
bool isSoftKeyword(const std::string& word) {
  static constexpr std::string_view kSoft[] = {"BEGIN", "COMMIT", "EXPLAIN", "INDEX", "KEY", "ROLLBACK", "TRANSACTION"};
  return std::find(std::begin(kSoft), std::end(kSoft), std::string_view(word)) != std::end(kSoft);
}

class Parser {
 public:
  explicit Parser(std::string_view sql) : tokens_(tokenize(sql)) {}

  std::vector<ast::Statement> parseAll() {
    std::vector<ast::Statement> statements;
    for (;;) {
      while (matchSymbol(";")) {
      }
      if (peek().kind == TokenKind::End) return statements;
      statements.push_back(parseStatement());
      if (!isSymbol(";") && peek().kind != TokenKind::End) fail("expected end of statement");
    }
  }

  ExprPtr parseOnlyExpression() {
    ExprPtr expr = parseExpr();
    if (peek().kind != TokenKind::End) fail("expected end of expression");
    return expr;
  }

 private:
  struct DepthGuard {
    Parser& parser;
    explicit DepthGuard(Parser& p) : parser(p) {
      if (++parser.depth_ > kMaxParseDepth) parser.fail("expression nested too deeply");
    }
    ~DepthGuard() { --parser.depth_; }
  };

  // ---- token helpers -------------------------------------------------------

  const Token& peek(size_t ahead = 0) const {
    size_t i = std::min(index_ + ahead, tokens_.size() - 1);
    return tokens_[i];
  }
  const Token& advance() {
    const Token& token = peek();
    if (index_ + 1 < tokens_.size()) ++index_;
    return token;
  }
  bool isKeyword(std::string_view word, size_t ahead = 0) const {
    const Token& t = peek(ahead);
    return t.kind == TokenKind::Keyword && t.text == word;
  }
  bool isSymbol(std::string_view symbol, size_t ahead = 0) const {
    const Token& t = peek(ahead);
    return t.kind == TokenKind::Symbol && t.text == symbol;
  }
  bool matchKeyword(std::string_view word) {
    if (!isKeyword(word)) return false;
    advance();
    return true;
  }
  bool matchSymbol(std::string_view symbol) {
    if (!isSymbol(symbol)) return false;
    advance();
    return true;
  }
  void expectKeyword(std::string_view word) {
    if (!matchKeyword(word)) fail("expected " + std::string(word));
  }
  void expectSymbol(std::string_view symbol) {
    if (!matchSymbol(symbol)) fail("expected '" + std::string(symbol) + "'");
  }

  [[noreturn]] void fail(const std::string& message) const {
    const Token& t = peek();
    std::string found = t.kind == TokenKind::End ? "end of input" : "'" + t.text + "'";
    throw Error(ErrorKind::Syntax, message + ", found " + found, t.pos);
  }

  bool atName(size_t ahead = 0) const {
    const Token& t = peek(ahead);
    return t.kind == TokenKind::Identifier || t.kind == TokenKind::QuotedIdentifier ||
           (t.kind == TokenKind::Keyword && isSoftKeyword(t.text));
  }
  std::string parseName(const char* what) {
    if (!atName()) fail(std::string("expected ") + what);
    return advance().text;
  }

  // ---- statements ----------------------------------------------------------

  ast::Statement parseStatement() {
    SourcePos pos = peek().pos;
    if (isKeyword("EXPLAIN")) {
      advance();
      bool explainable = isKeyword("SELECT") || isKeyword("INSERT") || isKeyword("UPDATE") || isKeyword("DELETE");
      if (!explainable) fail("EXPLAIN works on SELECT, INSERT, UPDATE and DELETE");
      auto inner = std::make_unique<ast::Statement>(parseStatement());
      return ast::Statement{ast::Explain{std::move(inner)}, pos};
    }
    return ast::Statement{parseStatementBody(), pos};
  }

  ast::Statement::Node parseStatementBody() {
    if (isKeyword("SELECT")) return parseSelect();
    if (isKeyword("INSERT")) return parseInsert();
    if (isKeyword("UPDATE")) return parseUpdate();
    if (isKeyword("DELETE")) return parseDelete();
    if (isKeyword("CREATE")) return parseCreate();
    if (isKeyword("DROP")) return parseDrop();
    if (matchKeyword("BEGIN")) {
      matchKeyword("TRANSACTION");
      return ast::Begin{};
    }
    if (matchKeyword("COMMIT")) {
      matchKeyword("TRANSACTION");
      return ast::Commit{};
    }
    if (matchKeyword("ROLLBACK")) {
      matchKeyword("TRANSACTION");
      return ast::Rollback{};
    }
    fail("expected a statement");
  }

  ast::Statement::Node parseCreate() {
    expectKeyword("CREATE");
    if (matchKeyword("TABLE")) return parseCreateTable();
    bool unique = matchKeyword("UNIQUE");
    if (matchKeyword("INDEX")) return parseCreateIndex(unique);
    fail(unique ? "expected INDEX" : "expected TABLE, INDEX or UNIQUE INDEX");
  }

  bool parseIfNotExists() {
    if (!matchKeyword("IF")) return false;
    expectKeyword("NOT");
    expectKeyword("EXISTS");
    return true;
  }

  bool parseIfExists() {
    if (!matchKeyword("IF")) return false;
    expectKeyword("EXISTS");
    return true;
  }

  ast::CreateTable parseCreateTable() {
    ast::CreateTable create;
    create.ifNotExists = parseIfNotExists();
    create.name = parseName("a table name");
    expectSymbol("(");
    do {
      create.columns.push_back(parseColumnDef());
    } while (matchSymbol(","));
    expectSymbol(")");
    return create;
  }

  ast::ColumnDef parseColumnDef() {
    ast::ColumnDef column{};
    column.pos = peek().pos;
    column.name = parseName("a column name");
    column.type = parseType();
    for (;;) {
      if (matchKeyword("PRIMARY")) {
        expectKeyword("KEY");
        column.primaryKey = true;
      } else if (matchKeyword("NOT")) {
        expectKeyword("NULL");
        column.notNull = true;
      } else if (matchKeyword("NULL")) {
        // Explicitly nullable, which is the default anyway.
      } else if (matchKeyword("DEFAULT")) {
        // Parsed below predicate level so a following NOT NULL is not taken for NOT IN.
        column.defaultExpr = parseConcat();
      } else {
        return column;
      }
    }
  }

  Type parseType() {
    const Token& token = peek();
    if (token.kind != TokenKind::Identifier) fail("expected a type name");
    auto type = typeFromName(token.text);
    if (!type) fail("unknown type");
    advance();
    if (matchSymbol("(")) {  // VARCHAR(20): the length is accepted and not enforced
      if (peek().kind != TokenKind::IntLiteral) fail("expected a length");
      advance();
      expectSymbol(")");
    }
    return *type;
  }

  ast::CreateIndex parseCreateIndex(bool unique) {
    ast::CreateIndex create{};
    create.unique = unique;
    create.ifNotExists = parseIfNotExists();
    create.name = parseName("an index name");
    expectKeyword("ON");
    create.table = parseName("a table name");
    expectSymbol("(");
    create.columnPos = peek().pos;
    create.column = parseName("a column name");
    if (isSymbol(",")) fail("indexes cover a single column");
    expectSymbol(")");
    return create;
  }

  ast::Statement::Node parseDrop() {
    expectKeyword("DROP");
    if (matchKeyword("TABLE")) {
      bool ifExists = parseIfExists();
      return ast::DropTable{parseName("a table name"), ifExists};
    }
    if (matchKeyword("INDEX")) {
      bool ifExists = parseIfExists();
      return ast::DropIndex{parseName("an index name"), ifExists};
    }
    fail("expected TABLE or INDEX");
  }

  ast::Insert parseInsert() {
    expectKeyword("INSERT");
    expectKeyword("INTO");
    ast::Insert insert;
    insert.table = parseName("a table name");
    if (matchSymbol("(")) {
      do {
        insert.columns.push_back(parseName("a column name"));
      } while (matchSymbol(","));
      expectSymbol(")");
    }
    expectKeyword("VALUES");
    do {
      insert.rowPositions.push_back(peek().pos);
      expectSymbol("(");
      std::vector<ExprPtr> row;
      do {
        row.push_back(parseExpr());
      } while (matchSymbol(","));
      expectSymbol(")");
      insert.rows.push_back(std::move(row));
    } while (matchSymbol(","));
    return insert;
  }

  ast::Update parseUpdate() {
    expectKeyword("UPDATE");
    ast::Update update;
    update.table = parseName("a table name");
    expectKeyword("SET");
    do {
      ast::Assignment assignment;
      assignment.pos = peek().pos;
      assignment.column = parseName("a column name");
      expectSymbol("=");
      assignment.value = parseExpr();
      update.assignments.push_back(std::move(assignment));
    } while (matchSymbol(","));
    if (matchKeyword("WHERE")) update.where = parseExpr();
    return update;
  }

  ast::Delete parseDelete() {
    expectKeyword("DELETE");
    expectKeyword("FROM");
    ast::Delete del;
    del.table = parseName("a table name");
    if (matchKeyword("WHERE")) del.where = parseExpr();
    return del;
  }

  ast::Select parseSelect() {
    expectKeyword("SELECT");
    ast::Select select;
    if (isKeyword("DISTINCT") || isKeyword("ALL")) fail("SELECT DISTINCT and SELECT ALL are not supported");
    do {
      select.items.push_back(parseSelectItem());
    } while (matchSymbol(","));

    if (matchKeyword("FROM")) parseFrom(select);
    if (matchKeyword("WHERE")) select.where = parseExpr();
    if (matchKeyword("GROUP")) {
      expectKeyword("BY");
      do {
        select.groupBy.push_back(parseExpr());
      } while (matchSymbol(","));
    }
    if (matchKeyword("HAVING")) select.having = parseExpr();
    if (matchKeyword("ORDER")) {
      expectKeyword("BY");
      do {
        ExprPtr expr = parseExpr();
        bool descending = false;
        if (matchKeyword("DESC")) {
          descending = true;
        } else {
          matchKeyword("ASC");
        }
        select.orderBy.push_back(ast::OrderTerm{std::move(expr), descending});
      } while (matchSymbol(","));
    }
    if (matchKeyword("LIMIT")) select.limit = parseExpr();
    if (matchKeyword("OFFSET")) select.offset = parseExpr();
    return select;
  }

  ast::SelectItem parseSelectItem() {
    ast::SelectItem item;
    item.pos = peek().pos;
    if (matchSymbol("*")) return item;
    if (atName() && isSymbol(".", 1) && isSymbol("*", 2)) {
      item.starTable = advance().text;
      advance();
      advance();
      return item;
    }
    item.expr = parseExpr();
    item.alias = parseOptionalAlias();
    return item;
  }

  std::string parseOptionalAlias() {
    if (matchKeyword("AS")) return parseName("an alias");
    // Without AS only plain identifiers count, so a following keyword stays a keyword.
    TokenKind kind = peek().kind;
    if (kind == TokenKind::Identifier || kind == TokenKind::QuotedIdentifier) return advance().text;
    return {};
  }

  ast::TableRef parseTableRef() {
    ast::TableRef ref;
    ref.pos = peek().pos;
    ref.name = parseName("a table name");
    ref.alias = parseOptionalAlias();
    return ref;
  }

  void parseFrom(ast::Select& select) {
    select.from = parseTableRef();
    for (;;) {
      if (matchSymbol(",")) {
        select.joins.push_back(ast::Join{parseTableRef(), nullptr});
      } else if (matchKeyword("CROSS")) {
        expectKeyword("JOIN");
        select.joins.push_back(ast::Join{parseTableRef(), nullptr});
      } else if (isKeyword("JOIN") || isKeyword("INNER")) {
        if (matchKeyword("INNER") && !isKeyword("JOIN")) fail("expected JOIN");
        advance();
        ast::TableRef table = parseTableRef();
        expectKeyword("ON");
        select.joins.push_back(ast::Join{std::move(table), parseExpr()});
      } else {
        return;
      }
    }
  }

  // ---- expressions, loosest binding first ----------------------------------

  ExprPtr makeExpr(Expr::Node node, SourcePos pos, int childHeight) const {
    if (childHeight + 1 > kMaxExprHeight) throw Error(ErrorKind::Syntax, "expression nested too deeply", pos);
    return std::make_unique<Expr>(Expr{std::move(node), pos, childHeight + 1});
  }
  ExprPtr makeBinary(ast::BinaryOp op, ExprPtr lhs, ExprPtr rhs, SourcePos pos) const {
    int height = std::max(lhs->height, rhs->height);
    return makeExpr(ast::Binary{op, std::move(lhs), std::move(rhs)}, pos, height);
  }

  ExprPtr parseExpr() {
    DepthGuard guard(*this);
    return parseOr();
  }

  ExprPtr parseOr() {
    ExprPtr lhs = parseAnd();
    while (isKeyword("OR")) {
      SourcePos pos = advance().pos;
      lhs = makeBinary(ast::BinaryOp::Or, std::move(lhs), parseAnd(), pos);
    }
    return lhs;
  }

  ExprPtr parseAnd() {
    ExprPtr lhs = parseNot();
    while (isKeyword("AND")) {
      SourcePos pos = advance().pos;
      lhs = makeBinary(ast::BinaryOp::And, std::move(lhs), parseNot(), pos);
    }
    return lhs;
  }

  ExprPtr parseNot() {
    if (isKeyword("NOT")) {
      DepthGuard guard(*this);
      SourcePos pos = advance().pos;
      ExprPtr operand = parseNot();
      int height = operand->height;
      return makeExpr(ast::Unary{ast::UnaryOp::Not, std::move(operand)}, pos, height);
    }
    return parsePredicate();
  }

  std::optional<ast::BinaryOp> comparisonAtCursor() const {
    if (peek().kind != TokenKind::Symbol) return std::nullopt;
    const std::string& s = peek().text;
    if (s == "=") return ast::BinaryOp::Eq;
    if (s == "<>" || s == "!=") return ast::BinaryOp::Ne;
    if (s == "<") return ast::BinaryOp::Lt;
    if (s == "<=") return ast::BinaryOp::Le;
    if (s == ">") return ast::BinaryOp::Gt;
    if (s == ">=") return ast::BinaryOp::Ge;
    return std::nullopt;
  }

  ExprPtr parsePredicate() {
    ExprPtr lhs = parseConcat();
    for (;;) {
      SourcePos pos = peek().pos;
      if (auto op = comparisonAtCursor()) {
        advance();
        lhs = makeBinary(*op, std::move(lhs), parseConcat(), pos);
      } else if (matchKeyword("IS")) {
        bool negated = matchKeyword("NOT");
        expectKeyword("NULL");
        int height = lhs->height;
        lhs = makeExpr(ast::IsNull{std::move(lhs), negated}, pos, height);
      } else if (isKeyword("IN") || isKeyword("BETWEEN") || isKeyword("LIKE")) {
        lhs = parseSetOrRangePredicate(std::move(lhs), false, pos);
      } else if (isKeyword("NOT") && (isKeyword("IN", 1) || isKeyword("BETWEEN", 1) || isKeyword("LIKE", 1))) {
        advance();
        lhs = parseSetOrRangePredicate(std::move(lhs), true, pos);
      } else {
        return lhs;
      }
    }
  }

  ExprPtr parseSetOrRangePredicate(ExprPtr operand, bool negated, SourcePos pos) {
    if (matchKeyword("IN")) {
      expectSymbol("(");
      if (isKeyword("SELECT")) fail("subqueries are not supported");
      std::vector<ExprPtr> items;
      int height = operand->height;
      do {
        items.push_back(parseExpr());
        height = std::max(height, items.back()->height);
      } while (matchSymbol(","));
      expectSymbol(")");
      return makeExpr(ast::InList{std::move(operand), std::move(items), negated}, pos, height);
    }
    if (matchKeyword("BETWEEN")) {
      ExprPtr low = parseConcat();
      expectKeyword("AND");
      ExprPtr high = parseConcat();
      int height = std::max({operand->height, low->height, high->height});
      return makeExpr(ast::Between{std::move(operand), std::move(low), std::move(high), negated}, pos, height);
    }
    expectKeyword("LIKE");
    ExprPtr pattern = parseConcat();
    int height = std::max(operand->height, pattern->height);
    return makeExpr(ast::Like{std::move(operand), std::move(pattern), negated}, pos, height);
  }

  ExprPtr parseConcat() {
    ExprPtr lhs = parseAdditive();
    while (isSymbol("||")) {
      SourcePos pos = advance().pos;
      lhs = makeBinary(ast::BinaryOp::Concat, std::move(lhs), parseAdditive(), pos);
    }
    return lhs;
  }

  ExprPtr parseAdditive() {
    ExprPtr lhs = parseMultiplicative();
    while (isSymbol("+") || isSymbol("-")) {
      auto op = isSymbol("+") ? ast::BinaryOp::Add : ast::BinaryOp::Sub;
      SourcePos pos = advance().pos;
      lhs = makeBinary(op, std::move(lhs), parseMultiplicative(), pos);
    }
    return lhs;
  }

  ExprPtr parseMultiplicative() {
    ExprPtr lhs = parseUnary();
    while (isSymbol("*") || isSymbol("/") || isSymbol("%")) {
      auto op = isSymbol("*") ? ast::BinaryOp::Mul : isSymbol("/") ? ast::BinaryOp::Div : ast::BinaryOp::Mod;
      SourcePos pos = advance().pos;
      lhs = makeBinary(op, std::move(lhs), parseUnary(), pos);
    }
    return lhs;
  }

  ExprPtr parseUnary() {
    if (isSymbol("-") || isSymbol("+")) {
      DepthGuard guard(*this);
      bool negate = isSymbol("-");
      SourcePos pos = advance().pos;
      // Folded here so that -9223372036854775808 is a valid literal even
      // though its magnitude alone does not fit in an INT.
      if (negate && peek().kind == TokenKind::IntLiteral) return parseIntLiteral(true, pos);
      ExprPtr operand = parseUnary();
      int height = operand->height;
      auto op = negate ? ast::UnaryOp::Negate : ast::UnaryOp::Plus;
      return makeExpr(ast::Unary{op, std::move(operand)}, pos, height);
    }
    return parsePrimary();
  }

  ExprPtr parseIntLiteral(bool negative, SourcePos pos) {
    const Token& token = peek();
    auto value = parseInt(negative ? "-" + token.text : token.text);
    if (!value) throw Error(ErrorKind::Syntax, "integer literal out of range", token.pos);
    advance();
    return makeExpr(ast::Literal{Value::Int(*value)}, pos, 0);
  }

  ExprPtr parsePrimary() {
    const Token& token = peek();
    SourcePos pos = token.pos;
    switch (token.kind) {
      case TokenKind::IntLiteral:
        return parseIntLiteral(false, pos);
      case TokenKind::FloatLiteral: {
        auto value = parseFloat(token.text);
        if (!value) throw Error(ErrorKind::Syntax, "number out of range", pos);
        advance();
        return makeExpr(ast::Literal{Value::Float(*value)}, pos, 0);
      }
      case TokenKind::StringLiteral: {
        Value value = Value::Text(token.text);
        advance();
        return makeExpr(ast::Literal{std::move(value)}, pos, 0);
      }
      default:
        break;
    }
    if (matchKeyword("NULL")) return makeExpr(ast::Literal{Value::Null()}, pos, 0);
    if (matchKeyword("TRUE")) return makeExpr(ast::Literal{Value::Bool(true)}, pos, 0);
    if (matchKeyword("FALSE")) return makeExpr(ast::Literal{Value::Bool(false)}, pos, 0);
    if (matchKeyword("CAST")) return parseCast(pos);
    if (matchSymbol("(")) {
      if (isKeyword("SELECT")) fail("subqueries are not supported");
      ExprPtr inner = parseExpr();
      expectSymbol(")");
      return inner;
    }
    if (atName()) {
      if (isSymbol("(", 1) && token.kind != TokenKind::QuotedIdentifier) return parseCall(pos);
      std::string first = advance().text;
      if (matchSymbol(".")) return makeExpr(ast::ColumnRef{std::move(first), parseName("a column name")}, pos, 0);
      return makeExpr(ast::ColumnRef{"", std::move(first)}, pos, 0);
    }
    fail("expected an expression");
  }

  ExprPtr parseCast(SourcePos pos) {
    expectSymbol("(");
    ExprPtr operand = parseExpr();
    expectKeyword("AS");
    Type target = parseType();
    expectSymbol(")");
    int height = operand->height;
    return makeExpr(ast::Cast{std::move(operand), target}, pos, height);
  }

  ExprPtr parseCall(SourcePos pos) {
    std::string name = toUpperAscii(advance().text);
    expectSymbol("(");
    if (matchSymbol("*")) {
      expectSymbol(")");
      return makeExpr(ast::Call{std::move(name), {}, true}, pos, 0);
    }
    if (isKeyword("DISTINCT")) fail("DISTINCT inside aggregates is not supported");
    std::vector<ExprPtr> args;
    int height = 0;
    if (!isSymbol(")")) {
      do {
        args.push_back(parseExpr());
        height = std::max(height, args.back()->height);
      } while (matchSymbol(","));
    }
    expectSymbol(")");
    return makeExpr(ast::Call{std::move(name), std::move(args), false}, pos, height);
  }

  std::vector<Token> tokens_;
  size_t index_ = 0;
  int depth_ = 0;
};

}  // namespace

std::vector<ast::Statement> parseScript(std::string_view sql) { return Parser(sql).parseAll(); }

ast::ExprPtr parseExpression(std::string_view sql) { return Parser(sql).parseOnlyExpression(); }

}  // namespace minisql
