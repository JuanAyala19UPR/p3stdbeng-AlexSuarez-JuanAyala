# sql-ddl

## Purpose

Gives the session lifecycle control over tables and indexes: CREATE and DROP statements that persist as catalog entries and on-disk files, through the engine's catalog writer — the DDL mutation seam.

## Requirements

### Requirement: CREATE TABLE registers a table
`CREATE TABLE <name> (<col> <type>, ...)` SHALL create the table's catalog entry — one JSON catalog file per table, holding the table name and the columns in declaration order — with `INTEGER`/`INT` mapping to a 4-byte integer column, `VARCHAR(n)` to a string column of declared width n, and `DOUBLE` to an 8-byte double column. After success the table is immediately usable by the other statements. Creating a table whose name already exists in the catalog SHALL fail with a named error and leave the existing catalog untouched.

#### Scenario: New table becomes usable
- **WHEN** `CREATE TABLE person (pid INTEGER, name VARCHAR(10), age INT, city VARCHAR(3))` succeeds
- **THEN** the catalog file `tablecatalog/person.json` exists, and an INSERT into `person` is accepted without further setup

#### Scenario: Duplicate name refused
- **WHEN** `CREATE TABLE person ...` runs while `tablecatalog/person.json` already exists
- **THEN** the statement fails with a named error and the existing catalog file is unchanged

### Requirement: DROP TABLE removes a table, its files, and its indexes
`DROP TABLE <name>` SHALL remove the table's catalog entry, erase its data file from disk, and erase every index file belonging to that table. A data file (or index file) the current session holds open SHALL be closed by the session as part of the drop — the engine refuses to erase a file still open through it, so the session releases its own handles first, and a handle held outside the session's control refuses the drop. Dropping a name with no catalog entry SHALL fail with a named error. After success, statements naming the table report it as unknown.

#### Scenario: Table vanishes completely
- **WHEN** table `person` has a data file and an index file, and `DROP TABLE person` succeeds
- **THEN** the catalog entry, the data file, and the index file are all gone from disk

#### Scenario: Drop closes the session's own open file
- **WHEN** a table's data file was opened earlier in the same session and `DROP TABLE` names that table
- **THEN** the session's handle (and its open index files) are closed first, all files are removed, and later statements naming the table report an unknown table

#### Scenario: Unknown table refused
- **WHEN** `DROP TABLE nosuch` runs with no such catalog entry
- **THEN** the statement fails with a named error and nothing on disk changes

### Requirement: CREATE INDEX builds and records an index
`CREATE INDEX <name> ON <table> (<column>) [USING ISAM | BTREE]` SHALL record the index — name, column, and type — in the table's catalog entry and build the index over the table's current rows as a tree of the declared type (`ISAM` when the clause is absent). An unknown type SHALL be refused with a named error. Creating an index on an unknown table or column SHALL fail with a named error. A second index on the same column of the same table SHALL be refused with a named error regardless of type (one index per column, whatever its kind). A failed CREATE INDEX SHALL leave the catalog unchanged.

#### Scenario: Index over existing rows
- **WHEN** table `person` holds 100 rows and `CREATE INDEX person_pid ON person (pid)` succeeds
- **THEN** the catalog entry for `person` lists index `person_pid` on column `pid` with type `ISAM` (the default), and a SELECT filtering on `pid` can use the index path

#### Scenario: Explicit B+tree type recorded
- **WHEN** `CREATE INDEX person_pid ON person (pid) USING BTREE` succeeds
- **THEN** the catalog entry records the index with type `BTREE`, the index file is a B+tree file, and a SELECT filtering on `pid` uses the index path through the B+tree

#### Scenario: Unknown type refused
- **WHEN** `CREATE INDEX person_pid ON person (pid) USING HASH` runs
- **THEN** the statement fails with a named error naming the unsupported type and the catalog is unchanged

#### Scenario: One index per column
- **WHEN** `person (pid)` already carries an ISAM index and `CREATE INDEX other ON person (pid) USING BTREE` runs
- **THEN** the statement fails with a named error and the catalog still lists exactly one index on `pid`, whatever its type
### Requirement: Index files live beside their data file
An index's physical file SHALL be named after the data file and column (`<data-file>.<column>.idx`) and sit beside it, whatever the index's type; the file's own magic SHALL identify which kind was built. When a data file is opened for a table whose catalog declares indexes and an expected index file is missing, it SHALL be built from that data file's current rows as the catalog-declared type rather than reported as an error. Opening an existing index file SHALL verify the file's kind matches the catalog-declared type; a mismatch SHALL be refused with a named error.

#### Scenario: Index discovered in a later session
- **WHEN** an index was created in an earlier session and a new session opens the same data file
- **THEN** the index is used from its existing file without rebuilding — as the type the catalog declares — and lookups answer from it

#### Scenario: Missing index file rebuilt
- **WHEN** a data file's `.pid.idx` was deleted externally and the data file is opened again
- **THEN** the index file is rebuilt from the data file's rows as the catalog-declared type and answers are correct again

#### Scenario: File kind contradicting the catalog refused
- **WHEN** the catalog declares `pid`'s index as a B+tree but `data/person.tbl.pid.idx` on disk is an ISAM file
- **THEN** opening the index fails with a named error and no entries are returned
### Requirement: DROP INDEX removes an index
`DROP INDEX <name>` SHALL remove the index's catalog entry and erase its file(s) from disk. Dropping an unknown index name SHALL fail with a named error. After success, SELECTs on the table no longer report an index path for that column.

#### Scenario: Dropped index leaves heap path only
- **WHEN** index `person_pid` is dropped and a SELECT filters on `pid` afterwards
- **THEN** the SELECT succeeds via the heap path and prints the heap-scan marker

### Requirement: Catalog writes are all-or-nothing
Every DDL statement that alters a catalog SHALL either fully apply or leave the catalog exactly as before; a mid-write failure SHALL never leave a catalog file that fails to load.

#### Scenario: Catalog survives failed DDL
- **WHEN** a DDL statement fails partway (for example an unwritable target)
- **THEN** the catalog file still loads and describes the state before the failed statement

### Requirement: Canonical file layout
A table's data file SHALL be `data/<table>.tbl` in the working directory, and the `data` directory SHALL be created on demand. An index file SHALL sit beside its data file as `data/<table>.tbl.<column>.idx`. Every SQL operation that resolves a table's files — CREATE TABLE and DROP TABLE file lifecycle, INSERT, SELECT, lazy index rebuild, and index verification — SHALL use these paths. The catalog remains at `tablecatalog/<name>.json`; no other data-file naming exists (in particular, no catalog key redirects a table's data file elsewhere).

#### Scenario: Table data lands in data/
- **WHEN** `CREATE TABLE parts ...` succeeds and rows are inserted
- **THEN** the rows live in `data/parts.tbl`, and no `parts.bin` appears in the working directory

#### Scenario: Index beside its table
- **WHEN** `CREATE INDEX parts_pid ON parts (pid)` succeeds
- **THEN** the index file is `data/parts.tbl.pid.idx`

#### Scenario: Drop removes everything it owns
- **WHEN** `DROP TABLE parts` succeeds
- **THEN** `data/parts.tbl` and every `data/parts.tbl.<column>.idx` are gone from disk, and the working directory never held a `parts.bin`
