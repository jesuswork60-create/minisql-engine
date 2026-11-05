#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace minisql {

struct CsvOptions {
  char delimiter = ',';
  bool header = true;
};

struct CsvField {
  std::string text;
  bool quoted = false;  // an unquoted empty field means NULL, a quoted one ""
};

// Reads RFC 4180 records: quoted fields may hold delimiters, doubled quotes and
// newlines; LF and CRLF line ends both work; blank lines are skipped.
class CsvReader {
 public:
  CsvReader(std::istream& in, char delimiter) : in_(in), delimiter_(delimiter) {}

  // False at end of input. Throws an i/o Error on malformed quoting.
  bool readRecord(std::vector<CsvField>& fields);
  // Line on which the most recent record started.
  int recordLine() const { return recordLine_; }

 private:
  std::istream& in_;
  char delimiter_;
  int line_ = 1;
  int recordLine_ = 0;
  bool atStart_ = true;
};

// Quotes only when needed, and always quotes an empty string so it stays
// distinct from NULL, which is written as nothing at all.
std::string csvEscape(const std::string& text, char delimiter);

}  // namespace minisql
