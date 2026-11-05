#include "minisql/catalog.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

#include "minisql/ast.h"

namespace minisql {

// ---- HashIndex --------------------------------------------------------------

HashIndex::HashIndex(std::string name, size_t column, bool unique, bool primary)
    : name_(std::move(name)), column_(column), unique_(unique), primary_(primary) {}

const std::vector<RowId>* HashIndex::lookup(const Value& key) const {
  auto it = buckets_.find(key);
  return it == buckets_.end() ? nullptr : &it->second;
}

void HashIndex::add(const Value& key, RowId id) {
  if (!key.isNull()) buckets_[key].push_back(id);
}

void HashIndex::remove(const Value& key, RowId id) {
  if (key.isNull()) return;
  auto it = buckets_.find(key);
  if (it == buckets_.end()) return;
  auto& ids = it->second;
  ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
  if (ids.empty()) buckets_.erase(it);
}

// ---- Table ------------------------------------------------------------------

Table::Table(std::string name, std::vector<Column> columns) : name_(std::move(name)), columns_(std::move(columns)) {
  for (size_t i = 0; i < columns_.size(); ++i) {
    if (columns_[i].primaryKey) indexes_.emplace_back(name_ + "_pkey", i, true, true);
  }
}

std::optional<size_t> Table::columnIndex(const std::string& name) const {
  for (size_t i = 0; i < columns_.size(); ++i) {
    if (equalsIgnoreCase(columns_[i].name, name)) return i;
  }
  return std::nullopt;
}

Value Table::normalize(size_t column, const Value& value) const {
  const Column& def = columns_[column];
  if (value.isNull()) {
    if (def.notNull) {
      throw Error(ErrorKind::Constraint, "NOT NULL constraint failed: " + name_ + "." + def.name);
    }
    return value;
  }
  auto converted = assignValue(value, def.type);
  if (!converted) {
    throw Error(ErrorKind::Type, "column " + name_ + "." + def.name + " expects " +
                                     std::string(typeName(def.type)) + ", got " + value.toSql());
  }
  return *converted;
}

const HashIndex* Table::indexOn(size_t column) const {
  const HashIndex* best = nullptr;
  for (const HashIndex& index : indexes_) {
    if (index.column() != column) continue;
    if (!best || (index.unique() && !best->unique())) best = &index;
  }
  return best;
}

const HashIndex* Table::findIndex(const std::string& name) const {
  for (const HashIndex& index : indexes_) {
    if (equalsIgnoreCase(index.name(), name)) return &index;
  }
  return nullptr;
}

void Table::addIndex(const std::string& name, size_t column, bool unique) {
  HashIndex index(name, column, unique, false);
  for (RowId id = 0; id < rows_.size(); ++id) {
    if (!live_[id]) continue;
    const Value& key = rows_[id][column];
    if (unique && !key.isNull() && index.lookup(key)) throwDuplicate(index, key);
    index.add(key, id);
  }
  indexes_.push_back(std::move(index));
}

void Table::dropIndex(const std::string& name) {
  auto it = std::find_if(indexes_.begin(), indexes_.end(),
                         [&](const HashIndex& index) { return equalsIgnoreCase(index.name(), name); });
  if (it != indexes_.end()) indexes_.erase(it);
}

void Table::throwDuplicate(const HashIndex& index, const Value& key) const {
  throw Error(ErrorKind::Constraint, "UNIQUE constraint failed: " + name_ + "." + columns_[index.column()].name +
                                         " (duplicate value " + key.toSql() + ")");
}

void Table::insertRows(std::vector<Row> rows) {
  for (const HashIndex& index : indexes_) {
    if (!index.unique()) continue;
    std::unordered_set<Value, ValueHash, ValueKeyEq> seen;
    for (const Row& row : rows) {
      const Value& key = row[index.column()];
      if (key.isNull()) continue;
      if (index.lookup(key) || !seen.insert(key).second) throwDuplicate(index, key);
    }
  }
  for (Row& row : rows) {
    RowId id = rows_.size();
    for (HashIndex& index : indexes_) index.add(row[index.column()], id);
    rows_.push_back(std::move(row));
    live_.push_back(1);
    ++liveCount_;
  }
}

void Table::applyUpdates(std::vector<RowUpdate> updates) {
  // Uniqueness is judged on the final state, so UPDATE t SET id = id + 1 works
  // even though intermediate states would collide row by row.
  std::unordered_set<RowId> touched;
  for (const RowUpdate& update : updates) touched.insert(update.id);
  for (const HashIndex& index : indexes_) {
    if (!index.unique()) continue;
    std::unordered_set<Value, ValueHash, ValueKeyEq> seen;
    for (const RowUpdate& update : updates) {
      const Value& key = update.row[index.column()];
      if (key.isNull()) continue;
      if (!seen.insert(key).second) throwDuplicate(index, key);
      if (const auto* holders = index.lookup(key)) {
        for (RowId holder : *holders) {
          if (touched.count(holder) == 0) throwDuplicate(index, key);
        }
      }
    }
  }

  for (HashIndex& index : indexes_) {
    size_t column = index.column();
    for (const RowUpdate& update : updates) {
      const Value& before = rows_[update.id][column];
      if (!sameKey(before, update.row[column])) index.remove(before, update.id);
    }
    for (const RowUpdate& update : updates) {
      const Value& before = rows_[update.id][column];
      if (!sameKey(before, update.row[column])) index.add(update.row[column], update.id);
    }
  }
  for (RowUpdate& update : updates) rows_[update.id] = std::move(update.row);
}

void Table::deleteRows(const std::vector<RowId>& ids) {
  for (RowId id : ids) {
    for (HashIndex& index : indexes_) index.remove(rows_[id][index.column()], id);
    rows_[id].clear();
    live_[id] = 0;
    --liveCount_;
  }
  compactIfWorthwhile();
}

void Table::compactIfWorthwhile() {
  size_t dead = rows_.size() - liveCount_;
  if (dead < 64 || dead < liveCount_) return;
  std::vector<Row> kept;
  kept.reserve(liveCount_);
  for (RowId id = 0; id < rows_.size(); ++id) {
    if (live_[id]) kept.push_back(std::move(rows_[id]));
  }
  rows_ = std::move(kept);
  live_.assign(rows_.size(), 1);
  for (HashIndex& index : indexes_) {
    index.clear();
    for (RowId id = 0; id < rows_.size(); ++id) index.add(rows_[id][index.column()], id);
  }
}

std::string Table::createSql() const {
  std::string sql = "CREATE TABLE " + ast::quoteIdentifier(name_) + " (";
  for (size_t i = 0; i < columns_.size(); ++i) {
    const Column& column = columns_[i];
    if (i) sql += ", ";
    sql += ast::quoteIdentifier(column.name) + " " + std::string(typeName(column.type));
    if (column.primaryKey) {
      sql += " PRIMARY KEY";
    } else if (column.notNull) {
      sql += " NOT NULL";
    }
    if (column.hasDefault) sql += " DEFAULT " + column.defaultValue.toSql();
  }
  return sql + ");";
}

std::vector<std::string> Table::indexSql() const {
  std::vector<std::string> statements;
  for (const HashIndex& index : indexes_) {
    if (index.primary()) continue;
    statements.push_back(std::string("CREATE ") + (index.unique() ? "UNIQUE " : "") + "INDEX " +
                         ast::quoteIdentifier(index.name()) + " ON " + ast::quoteIdentifier(name_) + " (" +
                         ast::quoteIdentifier(columns_[index.column()].name) + ");");
  }
  return statements;
}

// ---- Catalog ----------------------------------------------------------------

const Table* Catalog::find(const std::string& name) const {
  auto it = tables_.find(toLowerAscii(name));
  return it == tables_.end() ? nullptr : it->second.get();
}

const Table& Catalog::require(const std::string& name, SourcePos pos) const {
  const Table* table = find(name);
  if (!table) throw Error(ErrorKind::Catalog, "no such table: " + name, pos);
  return *table;
}

Table& Catalog::mutableTable(const std::string& name, SourcePos pos) {
  auto it = tables_.find(toLowerAscii(name));
  if (it == tables_.end()) throw Error(ErrorKind::Catalog, "no such table: " + name, pos);
  // Copy on write: an open snapshot may still hold this table.
  if (it->second.use_count() > 1) it->second = std::make_shared<Table>(*it->second);
  return *it->second;
}

void Catalog::addTable(Table table) {
  std::string key = toLowerAscii(table.name());
  tables_[key] = std::make_shared<Table>(std::move(table));
}

void Catalog::dropTable(const std::string& name) { tables_.erase(toLowerAscii(name)); }

const Table* Catalog::tableWithIndex(const std::string& indexName) const {
  for (const auto& entry : tables_) {
    if (entry.second->findIndex(indexName)) return entry.second.get();
  }
  return nullptr;
}

std::vector<const Table*> Catalog::tables() const {
  std::vector<const Table*> out;
  out.reserve(tables_.size());
  for (const auto& entry : tables_) out.push_back(entry.second.get());
  return out;
}

}  // namespace minisql
