#include "minisql/shell.h"

#include <fstream>
#include <istream>
#include <ostream>
#include <sstream>
#include <vector>

#include "minisql/format.h"
#include "minisql/lexer.h"
#include "minisql/parser.h"

namespace minisql {

namespace {

constexpr int kMaxReadDepth = 16;

// Splits a dot command on spaces; quotes group words, as in a file name with spaces.
std::vector<std::string> splitWords(const std::string& line) {
  std::vector<std::string> words;
  std::string current;
  bool inWord = false;
  char quote = '\0';
  for (char c : line) {
    if (quote) {
      if (c == quote) {
        quote = '\0';
      } else {
        current += c;
      }
    } else if (c == '\'' || c == '"') {
      quote = c;
      inWord = true;
    } else if (c == ' ' || c == '\t' || c == '\r') {
      if (inWord) words.push_back(std::move(current));
      current.clear();
      inWord = false;
    } else {
      current += c;
      inWord = true;
    }
  }
  if (inWord) words.push_back(std::move(current));
  return words;
}

bool isBlank(const std::string& line) { return line.find_first_not_of(" \t\r") == std::string::npos; }

}  // namespace

Shell::Shell(Database& database, std::ostream& out, std::ostream& err, ShellOptions options)
    : database_(database), out_(out), err_(err), options_(options) {}

int Shell::runText(std::string_view text) {
  std::istringstream in{std::string(text)};
  return runStream(in);
}

int Shell::runStream(std::istream& in, const std::string& sourceName) {
  int status = 0;
  std::string buffer;
  int firstLine = 0;
  int lineNumber = 0;
  std::string line;

  auto prompt = [&] {
    if (options_.prompts) out_ << (buffer.empty() ? "minisql> " : "   ...> ") << std::flush;
  };

  prompt();
  while (!quit_ && std::getline(in, line)) {
    ++lineNumber;
    if (buffer.empty()) {
      size_t start = line.find_first_not_of(" \t");
      if (start != std::string::npos && line[start] == '.') {
        if (!runDotCommand(line.substr(start))) {
          status = 1;
          if (options_.stopOnError) return status;
        }
        prompt();
        continue;
      }
      if (isBlank(line)) {
        prompt();
        continue;
      }
      firstLine = lineNumber;
    }
    buffer += line;
    buffer += '\n';

    SqlScan scan = scanSql(buffer);
    if (!scan.hasContent && !scan.unterminated) {
      buffer.clear();  // nothing but comments so far
    } else if (scan.endsStatement) {
      bool ok = executeBuffer(buffer, firstLine, sourceName);
      buffer.clear();
      if (!ok) {
        status = 1;
        if (options_.stopOnError) return status;
      }
    }
    prompt();
  }

  // A last statement without its semicolon still runs; an unfinished one
  // reports its syntax error here.
  if (!quit_ && !buffer.empty() && !executeBuffer(buffer, firstLine, sourceName)) status = 1;
  return status;
}

bool Shell::executeBuffer(const std::string& sql, int firstLine, const std::string& sourceName) {
  auto report = [&](const Error& e) {
    int offset = sourceName.empty() ? 0 : firstLine - 1;
    if (!sourceName.empty()) err_ << sourceName << ": ";
    err_ << renderError(e, sql, offset) << '\n';
  };
  try {
    // Statements run one by one so that earlier output survives a later failure.
    for (const ast::Statement& statement : parseScript(sql)) printResult(database_.run(statement), out_);
    return true;
  } catch (const Error& e) {
    report(e);
    return false;
  }
}

bool Shell::runDotCommand(const std::string& line) {
  std::vector<std::string> words = splitWords(line);
  const std::string& command = words[0];
  size_t argCount = words.size() - 1;
  auto usage = [&](const char* text) {
    err_ << "usage: " << text << '\n';
    return false;
  };

  if (command == ".quit" || command == ".exit") {
    quit_ = true;
    return true;
  }
  if (command == ".help") {
    printHelp();
    return true;
  }
  if (command == ".tables") {
    for (const Table* table : database_.catalog().tables()) out_ << table->name() << '\n';
    return true;
  }
  if (command == ".schema") {
    if (argCount > 1) return usage(".schema [TABLE]");
    return commandSchema(argCount == 1 ? words[1] : "");
  }
  if (command == ".dump") {
    if (argCount > 1) return usage(".dump [FILE]");
    return commandDump(argCount == 1 ? words[1] : "");
  }
  if (command == ".import") {
    if (argCount != 2) return usage(".import FILE TABLE");
    return commandImport(words[1], words[2]);
  }
  if (command == ".export") {
    if (argCount != 2) return usage(".export TABLE FILE");
    return commandExport(words[1], words[2]);
  }
  if (command == ".read") {
    if (argCount != 1) return usage(".read FILE");
    return commandRead(words[1]);
  }
  err_ << "unknown command " << command << " (try .help)\n";
  return false;
}

void Shell::printHelp() {
  out_ << ".dump [FILE]         write the database as a SQL script\n"
          ".export TABLE FILE   write a table as CSV\n"
          ".help                show this list\n"
          ".import FILE TABLE   load CSV with a header row into a table, creating it if needed\n"
          ".quit                leave the shell\n"
          ".read FILE           run a SQL script\n"
          ".schema [TABLE]      show CREATE statements\n"
          ".tables              list tables\n";
}

bool Shell::commandSchema(const std::string& tableName) {
  const Catalog& catalog = database_.catalog();
  std::vector<const Table*> tables;
  if (tableName.empty()) {
    tables = catalog.tables();
  } else if (const Table* table = catalog.find(tableName)) {
    tables.push_back(table);
  } else {
    err_ << "no such table: " << tableName << '\n';
    return false;
  }
  for (const Table* table : tables) {
    out_ << table->createSql() << '\n';
    for (const std::string& statement : table->indexSql()) out_ << statement << '\n';
  }
  return true;
}

bool Shell::commandDump(const std::string& path) {
  if (path.empty()) {
    database_.dump(out_);
    return true;
  }
  std::ofstream file(path);
  if (!file) {
    err_ << "cannot write " << path << '\n';
    return false;
  }
  database_.dump(file);
  return true;
}

bool Shell::commandImport(const std::string& path, const std::string& table) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    err_ << "cannot read " << path << '\n';
    return false;
  }
  try {
    size_t count = database_.importCsv(table, file);
    out_ << "IMPORT " << count << '\n';
    return true;
  } catch (const Error& e) {
    err_ << path << ": " << e.what() << '\n';
    return false;
  }
}

bool Shell::commandExport(const std::string& table, const std::string& path) {
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    err_ << "cannot write " << path << '\n';
    return false;
  }
  try {
    size_t count = database_.exportCsv(table, file);
    out_ << "EXPORT " << count << '\n';
    return true;
  } catch (const Error& e) {
    err_ << e.what() << '\n';
    return false;
  }
}

bool Shell::commandRead(const std::string& path) {
  if (readDepth_ >= kMaxReadDepth) {
    err_ << "scripts are nested too deeply at " << path << '\n';
    return false;
  }
  std::ifstream file(path);
  if (!file) {
    err_ << "cannot read " << path << '\n';
    return false;
  }
  ++readDepth_;
  ShellOptions saved = options_;
  options_.prompts = false;
  int status = runStream(file, path);
  options_ = saved;
  --readDepth_;
  return status == 0;
}

}  // namespace minisql
