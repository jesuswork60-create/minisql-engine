#include <unistd.h>

#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "minisql/database.h"
#include "minisql/shell.h"

namespace {

constexpr const char* kUsage =
    "usage: minisql [-c SQL]... [FILE]...\n"
    "\n"
    "With no arguments, reads statements from standard input (a prompt appears\n"
    "when it is a terminal). -c runs the given text and each FILE runs as a\n"
    "script, in the order given and against the same in-memory database.\n"
    "\n"
    "  -c SQL      run SQL or a dot command\n"
    "  -h, --help  show this help\n";

struct Source {
  bool isFile;
  std::string value;
};

}  // namespace

int main(int argc, char** argv) {
  std::vector<Source> sources;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << kUsage;
      return 0;
    }
    if (arg == "-c") {
      if (++i == argc) {
        std::cerr << "minisql: -c needs an argument\n" << kUsage;
        return 2;
      }
      sources.push_back(Source{false, argv[i]});
    } else if (arg.size() > 1 && arg[0] == '-') {
      std::cerr << "minisql: unknown option " << arg << '\n' << kUsage;
      return 2;
    } else {
      sources.push_back(Source{true, std::move(arg)});
    }
  }

  minisql::Database database;

  if (sources.empty()) {
    minisql::ShellOptions options;
    options.prompts = isatty(STDIN_FILENO) != 0;
    minisql::Shell shell(database, std::cout, std::cerr, options);
    int status = shell.runStream(std::cin);
    if (options.prompts && !shell.quitRequested()) std::cout << '\n';
    return status;
  }

  minisql::ShellOptions options;
  options.stopOnError = true;
  minisql::Shell shell(database, std::cout, std::cerr, options);
  for (const Source& source : sources) {
    int status;
    if (source.isFile) {
      std::ifstream file(source.value);
      if (!file) {
        std::cerr << "minisql: cannot read " << source.value << '\n';
        return 1;
      }
      status = shell.runStream(file, source.value);
    } else {
      status = shell.runText(source.value);
    }
    if (status != 0) return status;
    if (shell.quitRequested()) break;
  }
  return 0;
}
