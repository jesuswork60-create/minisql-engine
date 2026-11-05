#include "minisql/format.h"

#include <algorithm>
#include <ostream>

namespace minisql {

namespace {

// Control characters would break the grid, so they are shown escaped.
std::string cellText(const Value& value) {
  std::string out;
  for (char c : value.toString()) {
    if (c == '\n') {
      out += "\\n";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c == '\r') {
      out += "\\r";
    } else {
      out += c;
    }
  }
  return out;
}

// Width in terminal columns, assuming one column per UTF-8 code point.
size_t displayWidth(const std::string& text) {
  return static_cast<size_t>(
      std::count_if(text.begin(), text.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

void printRule(const std::vector<size_t>& widths, std::ostream& out) {
  for (size_t width : widths) out << '+' << std::string(width + 2, '-');
  out << "+\n";
}

void printCell(const std::string& text, size_t width, bool rightAlign, std::ostream& out) {
  std::string padding(width - displayWidth(text), ' ');
  out << "| ";
  if (rightAlign) out << padding;
  out << text;
  if (!rightAlign) out << padding;
  out << ' ';
}

}  // namespace

void printResult(const Result& result, std::ostream& out) {
  if (!result.isQuery()) {
    out << result.status << '\n';
    return;
  }

  std::vector<size_t> widths;
  widths.reserve(result.columns.size());
  for (const std::string& name : result.columns) widths.push_back(displayWidth(name));

  std::vector<std::vector<std::string>> cells;
  cells.reserve(result.rows.size());
  for (const Row& row : result.rows) {
    std::vector<std::string> line;
    line.reserve(row.size());
    for (size_t i = 0; i < row.size(); ++i) {
      line.push_back(cellText(row[i]));
      widths[i] = std::max(widths[i], displayWidth(line.back()));
    }
    cells.push_back(std::move(line));
  }

  printRule(widths, out);
  for (size_t i = 0; i < result.columns.size(); ++i) printCell(result.columns[i], widths[i], false, out);
  out << "|\n";
  printRule(widths, out);
  for (size_t r = 0; r < cells.size(); ++r) {
    for (size_t i = 0; i < cells[r].size(); ++i) {
      printCell(cells[r][i], widths[i], result.rows[r][i].isNumeric(), out);
    }
    out << "|\n";
  }
  printRule(widths, out);
  out << '(' << cells.size() << (cells.size() == 1 ? " row)\n" : " rows)\n");
}

}  // namespace minisql
