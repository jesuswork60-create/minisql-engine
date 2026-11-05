#pragma once

#include <iosfwd>
#include <string>
#include <string_view>

#include "minisql/database.h"

namespace minisql {

struct ShellOptions {
  bool prompts = false;      // print prompts, for a person at a terminal
  bool stopOnError = false;  // give up at the first failure, for scripts
};

// Line-oriented front end: SQL statements that may span lines, plus dot
// commands. Output and errors go to the given streams, so tests can drive it.
class Shell {
 public:
  Shell(Database& database, std::ostream& out, std::ostream& err, ShellOptions options = {});

  // Reads to end of input or .quit. Returns 0, or 1 if anything failed.
  // sourceName labels errors and makes their line numbers relative to the stream.
  int runStream(std::istream& in, const std::string& sourceName = "");
  int runText(std::string_view text);

  bool quitRequested() const { return quit_; }

 private:
  bool executeBuffer(const std::string& sql, int firstLine, const std::string& sourceName);
  bool runDotCommand(const std::string& line);
  bool commandRead(const std::string& path);
  bool commandImport(const std::string& path, const std::string& table);
  bool commandExport(const std::string& table, const std::string& path);
  bool commandDump(const std::string& path);
  bool commandSchema(const std::string& table);
  void printHelp();

  Database& database_;
  std::ostream& out_;
  std::ostream& err_;
  ShellOptions options_;
  bool quit_ = false;
  int readDepth_ = 0;
};

}  // namespace minisql
