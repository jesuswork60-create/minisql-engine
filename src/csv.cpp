#include "minisql/csv.h"

#include <istream>
#include <optional>
#include <ostream>
#include <set>
#include <utility>

#include "minisql/database.h"

namespace minisql {

namespace {

[[noreturn]] void csvError(const std::string& message, int line) {
  throw Error(ErrorKind::Io, "CSV: " + message, SourcePos{line, 0});
}

// The narrowest type that holds every non-NULL value; TEXT when all are NULL.
Type inferColumnType(const std::vector<std::vector<CsvField>>& records, size_t column) {
  bool anyValue = false, allInt = true, allFloat = true, allBool = true;
  for (const auto& record : records) {
    const CsvField& field = record[column];
    if (field.text.empty() && !field.quoted) continue;
    anyValue = true;
    allInt = allInt && parseInt(field.text).has_value();
    allFloat = allFloat && parseFloat(field.text).has_value();
    allBool = allBool && (equalsIgnoreCase(field.text, "true") || equalsIgnoreCase(field.text, "false"));
  }
  if (!anyValue) return Type::Text;
  if (allInt) return Type::Int;
  if (allFloat) return Type::Float;
  return allBool ? Type::Bool : Type::Text;
}

Value fieldToValue(const CsvField& field, const Table& table, size_t column, int line) {
  Value value = field.text.empty() && !field.quoted ? Value::Null() : Value::Text(field.text);
  try {
    return table.normalize(column, castValue(value, table.columns()[column].type));
  } catch (const Error& e) {
    csvError(e.message(), line);
  }
}

}  // namespace

bool CsvReader::readRecord(std::vector<CsvField>& fields) {
  enum class State { FieldStart, Unquoted, Quoted, AfterQuote };
  State state = State::FieldStart;
  std::string current;
  bool anything = false;
  fields.clear();

  auto finishField = [&](bool quoted) {
    fields.push_back(CsvField{std::move(current), quoted});
    current.clear();
  };

  if (atStart_) {
    atStart_ = false;
    // Spreadsheet exports often begin with a UTF-8 byte order mark.
    if (in_.peek() == 0xEF) {
      char bom[3];
      in_.read(bom, 3);
      if (!(in_.gcount() == 3 && bom[1] == '\xBB' && bom[2] == '\xBF')) {
        in_.clear();
        in_.seekg(0);
      }
    }
  }

  recordLine_ = line_;
  int quoteLine = line_;
  char c;
  while (in_.get(c)) {
    if (c == '\n') ++line_;
    switch (state) {
      case State::FieldStart:
        if (c == '"') {
          state = State::Quoted;
          quoteLine = line_;
          anything = true;
          break;
        }
        state = State::Unquoted;
        [[fallthrough]];
      case State::Unquoted:
        if (c == '\r' && in_.peek() == '\n') break;
        if (c == '\n') {
          if (anything || !current.empty()) {
            finishField(false);
            return true;
          }
          // Blank line: keep looking for a record on the next one.
          state = State::FieldStart;
          recordLine_ = line_;
          break;
        }
        anything = true;
        if (c == delimiter_) {
          finishField(false);
          state = State::FieldStart;
        } else {
          current += c;
        }
        break;
      case State::Quoted:
        if (c == '"') {
          state = State::AfterQuote;
        } else {
          current += c;
        }
        break;
      case State::AfterQuote:
        if (c == '"') {
          current += '"';
          state = State::Quoted;
        } else if (c == delimiter_) {
          finishField(true);
          state = State::FieldStart;
        } else if (c == '\n') {
          finishField(true);
          return true;
        } else if (!(c == '\r' && in_.peek() == '\n')) {
          csvError("unexpected text after closing quote", recordLine_);
        }
        break;
    }
  }
  if (state == State::Quoted) csvError("unterminated quoted field", quoteLine);
  if (!anything && current.empty()) return false;
  finishField(state == State::AfterQuote);
  return true;
}

std::string csvEscape(const std::string& text, char delimiter) {
  bool needsQuotes = text.empty();
  for (char c : text) {
    needsQuotes = needsQuotes || c == delimiter || c == '"' || c == '\n' || c == '\r';
  }
  if (!needsQuotes) return text;
  std::string out = "\"";
  for (char c : text) {
    if (c == '"') out += '"';
    out += c;
  }
  return out + "\"";
}

size_t Database::importCsv(const std::string& tableName, std::istream& in, const CsvOptions& options) {
  CsvReader reader(in, options.delimiter);
  std::vector<CsvField> fields;

  std::vector<std::string> header;
  if (options.header) {
    if (!reader.readRecord(fields)) csvError("input is empty, expected a header", 1);
    std::set<std::string> seen;
    for (CsvField& field : fields) {
      if (!seen.insert(toLowerAscii(field.text)).second || field.text.empty()) {
        csvError("bad or repeated column name '" + field.text + "' in header", reader.recordLine());
      }
      header.push_back(std::move(field.text));
    }
  }

  std::vector<std::vector<CsvField>> records;
  std::vector<int> lines;
  while (reader.readRecord(fields)) {
    records.push_back(fields);
    lines.push_back(reader.recordLine());
  }

  size_t expected = !header.empty() ? header.size() : 0;
  if (const Table* existing = catalog_.find(tableName); existing && header.empty()) {
    expected = existing->columns().size();
  }
  for (size_t i = 0; i < records.size(); ++i) {
    if (expected != 0 && records[i].size() != expected) {
      csvError("expected " + std::to_string(expected) + " fields, found " + std::to_string(records[i].size()), lines[i]);
    }
  }

  // Built as a scratch table first, so bad data cannot leave a half-made table behind.
  std::optional<Table> created;
  if (!catalog_.find(tableName)) {
    if (header.empty()) throw Error(ErrorKind::Catalog, "no such table: " + tableName);
    std::vector<Column> columns;
    for (size_t i = 0; i < header.size(); ++i) {
      Column column;
      column.name = header[i];
      column.type = inferColumnType(records, i);
      columns.push_back(std::move(column));
    }
    created.emplace(tableName, std::move(columns));
  }
  const Table& table = created ? *created : catalog_.require(tableName);

  // targets[i] is the table column fed by the i-th CSV field.
  std::vector<size_t> targets;
  const auto& columns = table.columns();
  if (header.empty()) {
    for (size_t i = 0; i < columns.size(); ++i) targets.push_back(i);
  } else {
    for (const std::string& name : header) {
      auto column = table.columnIndex(name);
      if (!column) throw Error(ErrorKind::Catalog, "no such column: " + tableName + "." + name);
      targets.push_back(*column);
    }
  }

  std::vector<Row> rows;
  rows.reserve(records.size());
  for (size_t r = 0; r < records.size(); ++r) {
    Row row(columns.size());
    std::vector<bool> given(columns.size(), false);
    for (size_t i = 0; i < targets.size(); ++i) {
      row[targets[i]] = fieldToValue(records[r][i], table, targets[i], lines[r]);
      given[targets[i]] = true;
    }
    for (size_t c = 0; c < columns.size(); ++c) {
      if (given[c]) continue;
      Value fallback = columns[c].hasDefault ? columns[c].defaultValue : Value::Null();
      try {
        row[c] = table.normalize(c, fallback);
      } catch (const Error& e) {
        csvError(e.message(), lines[r]);
      }
    }
    rows.push_back(std::move(row));
  }

  size_t count = rows.size();
  if (created) {
    created->insertRows(std::move(rows));
    catalog_.addTable(std::move(*created));
  } else {
    catalog_.mutableTable(tableName).insertRows(std::move(rows));
  }
  return count;
}

size_t Database::exportCsv(const std::string& tableName, std::ostream& out, const CsvOptions& options) const {
  const Table& table = catalog_.require(tableName);
  auto writeField = [&](size_t index, const std::string& text) {
    if (index) out << options.delimiter;
    out << text;
  };
  if (options.header) {
    const auto& columns = table.columns();
    for (size_t i = 0; i < columns.size(); ++i) writeField(i, csvEscape(columns[i].name, options.delimiter));
    out << '\n';
  }
  size_t count = 0;
  for (RowId id = 0; id < table.slotCount(); ++id) {
    if (!table.isLive(id)) continue;
    const Row& row = table.row(id);
    for (size_t i = 0; i < row.size(); ++i) {
      writeField(i, row[i].isNull() ? std::string() : csvEscape(row[i].toString(), options.delimiter));
    }
    out << '\n';
    ++count;
  }
  return count;
}

}  // namespace minisql
