#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "minisql/error.h"
#include "minisql/value.h"

namespace minisql {

struct Column {
  std::string name;
  Type type = Type::Text;
  bool notNull = false;
  bool primaryKey = false;
  bool hasDefault = false;
  Value defaultValue;  // already converted to the column type
};

using RowId = size_t;

// Hash index over one column. NULLs are not stored: an equality lookup can
// never match NULL, and a unique index allows any number of them.
class HashIndex {
 public:
  HashIndex(std::string name, size_t column, bool unique, bool primary);

  const std::string& name() const { return name_; }
  size_t column() const { return column_; }
  bool unique() const { return unique_; }
  bool primary() const { return primary_; }

  // Row ids holding key, or nullptr when there are none.
  const std::vector<RowId>* lookup(const Value& key) const;
  void add(const Value& key, RowId id);
  void remove(const Value& key, RowId id);
  void clear() { buckets_.clear(); }

 private:
  std::string name_;
  size_t column_;
  bool unique_;
  bool primary_;
  std::unordered_map<Value, std::vector<RowId>, ValueHash, ValueKeyEq> buckets_;
};

struct RowUpdate {
  RowId id;
  Row row;
};

// Rows live in a vector addressed by RowId, in insertion order; deleted rows
// stay as tombstones until enough pile up to compact. Row ids are stable only
// within a single statement.
//
// The mutators are all-or-nothing: they check every row against every unique
// index before changing anything, so a failure leaves the table untouched.
class Table {
 public:
  Table(std::string name, std::vector<Column> columns);

  const std::string& name() const { return name_; }
  const std::vector<Column>& columns() const { return columns_; }
  std::optional<size_t> columnIndex(const std::string& name) const;

  size_t slotCount() const { return rows_.size(); }
  size_t rowCount() const { return liveCount_; }
  bool isLive(RowId id) const { return live_[id] != 0; }
  const Row& row(RowId id) const { return rows_[id]; }

  // Converts a value for storage in the column, enforcing its type and NOT NULL.
  Value normalize(size_t column, const Value& value) const;

  const std::vector<HashIndex>& indexes() const { return indexes_; }
  // The best index on a column: unique ones first, nullptr if there is none.
  const HashIndex* indexOn(size_t column) const;
  const HashIndex* findIndex(const std::string& name) const;
  void addIndex(const std::string& name, size_t column, bool unique);
  void dropIndex(const std::string& name);

  // Rows must already be normalized and have one value per column.
  void insertRows(std::vector<Row> rows);
  void applyUpdates(std::vector<RowUpdate> updates);
  void deleteRows(const std::vector<RowId>& ids);

  // CREATE TABLE statement that recreates the columns and constraints.
  std::string createSql() const;
  // CREATE INDEX statements for the indexes that are not implied by the table.
  std::vector<std::string> indexSql() const;

 private:
  [[noreturn]] void throwDuplicate(const HashIndex& index, const Value& key) const;
  void compactIfWorthwhile();

  std::string name_;
  std::vector<Column> columns_;
  std::vector<Row> rows_;
  std::vector<char> live_;
  size_t liveCount_ = 0;
  std::vector<HashIndex> indexes_;
};

// All tables, by case-insensitive name. Tables are shared with any open
// snapshot and copied the first time they are written afterwards, so taking a
// snapshot costs one small map copy no matter how much data there is.
class Catalog {
 public:
  using Snapshot = std::map<std::string, std::shared_ptr<Table>>;

  const Table* find(const std::string& name) const;
  // Throws a catalog error when there is no such table.
  const Table& require(const std::string& name, SourcePos pos = {}) const;
  Table& mutableTable(const std::string& name, SourcePos pos = {});

  void addTable(Table table);
  void dropTable(const std::string& name);
  // The table owning the named index, or nullptr.
  const Table* tableWithIndex(const std::string& indexName) const;
  // In name order.
  std::vector<const Table*> tables() const;

  Snapshot snapshot() const { return tables_; }
  void restore(Snapshot snapshot) { tables_ = std::move(snapshot); }

 private:
  Snapshot tables_;
};

}  // namespace minisql
