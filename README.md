# minisql

A small SQL engine in C++17 that keeps everything in memory. It has a lexer, a recursive-descent parser, a planner with hash indexes, pull-based executor operators, snapshot transactions, CSV import and export, and a command-line shell. There are no dependencies outside the standard library.

It is a compact engine to read, change and experiment with. Nothing is saved to disk unless you ask for a `.dump`.

## What it supports

- `CREATE TABLE`, `DROP TABLE`, `CREATE [UNIQUE] INDEX`, `DROP INDEX`, `INSERT`, `UPDATE`, `DELETE`, `SELECT`
- Column types `INT`, `FLOAT`, `TEXT`, `BOOL`, with `NOT NULL`, `PRIMARY KEY` and `DEFAULT`
- `WHERE`, `ORDER BY`, `LIMIT`, `OFFSET`, inner `JOIN` with table aliases, and comma joins
- `GROUP BY`, `HAVING`, and `COUNT`, `SUM`, `AVG`, `MIN`, `MAX`
- Arithmetic, comparisons, `AND`/`OR`/`NOT` with SQL's three-valued logic, `IN`, `BETWEEN`, `LIKE`, `IS NULL`
- `UPPER`, `LOWER`, `LENGTH`, `ABS`, `COALESCE`
- `BEGIN`, `COMMIT`, `ROLLBACK`
- `EXPLAIN` for any `SELECT`, `INSERT`, `UPDATE` or `DELETE`

Each statement either applies completely or changes nothing, so a multi-row `INSERT` that fails on its last row leaves the table as it was. Errors name the line and column they came from.

## Install

You need a C++17 compiler and CMake 3.20 or newer.

```sh
cmake -S . -B build
cmake --build build -j1
```

This produces the `build/minisql` binary and the `minisql` static library. There is no install target, so run the binary from `build/` or copy it where you like.

## Usage

Pass statements with `-c`. Each `-c` and each script file runs in order against the same database:

```sh
./build/minisql \
  -c "CREATE TABLE books (id INT PRIMARY KEY, title TEXT NOT NULL, year INT);" \
  -c "INSERT INTO books VALUES (1, 'The Salt Road', 1987), (2, 'Winter Ledger', 2004);" \
  -c "SELECT title FROM books WHERE year > 2000;" \
  -c "EXPLAIN SELECT title FROM books WHERE id = 2;"
```

```
CREATE TABLE
INSERT 2
+---------------+
| title         |
+---------------+
| Winter Ledger |
+---------------+
(1 row)
+--------------------------------------------+
| plan                                       |
+--------------------------------------------+
| Project [title]                            |
|   IndexScan books using books_pkey key=(2) |
+--------------------------------------------+
(2 rows)
```

`./build/minisql examples/library.sql` runs a longer script with a join, a `GROUP BY` and an index.

A mistake points at the place it happened, and the exit status is 1:

```
$ ./build/minisql -c "SELECT * FROM nowhere;"
catalog error at line 1, column 15: no such table: nowhere
  SELECT * FROM nowhere;
                ^
```

### The shell

With no arguments the binary reads from standard input, with a prompt when that is a terminal. A statement can span several lines and ends at its semicolon. These dot commands work at the start of a line, and also through `-c` and in scripts:

| Command | What it does |
| --- | --- |
| `.help` | list the commands |
| `.tables` | list the tables |
| `.schema [TABLE]` | show `CREATE` statements |
| `.dump [FILE]` | write the whole database as SQL, to the screen or a file |
| `.import FILE TABLE` | load a CSV file into a table, creating it if needed |
| `.export TABLE FILE` | write a table as CSV |
| `.read FILE` | run a script |
| `.quit`, `.exit` | leave the shell |

In the shell an error is printed and the session continues, and the exit status is 1 at the end. With `-c` and script files the run stops at the first error.

`.import` treats the first line as a header and matches columns by name. When the table does not exist it is created, with each column's type inferred from its values. An unquoted empty field is NULL and a quoted empty field (`""`) is an empty string, and `.export` writes them the same way, so tables round-trip.

### As a library

Link against the `minisql_lib` CMake target:

```cpp
#include <iostream>

#include "minisql/database.h"
#include "minisql/format.h"

int main() {
  minisql::Database db;
  db.execute("CREATE TABLE t (id INT PRIMARY KEY, name TEXT);"
             "INSERT INTO t VALUES (1, 'Ann'), (2, 'Bo');");
  minisql::printResult(db.query("SELECT name FROM t WHERE id = 2"), std::cout);
}
```

`execute` runs any number of statements and returns their results. `query` runs exactly one. Failures throw `minisql::Error`, which carries a kind and a source position.

## Running the tests

```sh
cmake -S . -B build && cmake --build build -j1 && ctest --test-dir build --output-on-failure
```

The tests use a small assert helper in `tests/test_util.h`, so there is nothing else to install. Each subsystem has its own test program, and a few more CTest entries run the real binary to check its output and exit status.

## Known limitations

- Everything lives in memory. `.dump` writes a script you can reload, but there is no storage engine, no journal and no recovery.
- Only one connection exists. There is no concurrency, and one open transaction at most, with no savepoints.
- `BEGIN` keeps a snapshot by table. The first write to a table inside a transaction copies that whole table, so it costs time and memory proportional to the table's size.
- Joins are inner only, in the order written. There is no `LEFT JOIN`, no join reordering and no hash join. A join runs as a nested loop, or an index lookup per outer row when an equality fits an index.
- There are no subqueries, `DISTINCT`, `UNION`, `CASE`, `ALTER TABLE`, views or `INSERT ... SELECT`.
- Primary keys and indexes cover one column. There is no composite key and no column-level `UNIQUE`; use `CREATE UNIQUE INDEX`.
- Indexes are hash indexes and only help equality. Range conditions, `IN` lists and `ORDER BY` always scan or sort. The planner has no statistics.
- Types are checked while running, not while planning. `WHERE name = 1` is an error only when a row reaches it, so it passes silently on an empty table. An indexed column is checked at the lookup instead, so `WHERE id = 'x'` fails even on an empty table.
- Text functions count bytes. `LENGTH('é')` is 2, `UPPER` and `LOWER` change ASCII letters only, and `_` in `LIKE` matches one byte. `LIKE` is case-sensitive and has no `ESCAPE`.
- Quoted identifiers are still case-insensitive. `"Col"` and `col` are the same column.
- `VARCHAR(n)` is accepted as `TEXT`, and the length is not enforced.
- `GROUP BY` takes expressions, not select aliases or positions, so `GROUP BY 1` groups by the constant 1. `HAVING` cannot use select aliases. Groups come out in first-seen order unless you add `ORDER BY`.
- `ORDER BY` always sorts the full input, even with a small `LIMIT`.
- Removing a row from a non-unique index is linear in the number of rows sharing its value.

## License

MIT. See `LICENSE`.
